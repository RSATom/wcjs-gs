#include "JsPlayer.h"

#include <cassert>
#include <optional>

#include <glib.h>


namespace {

enum class StreamType
{
	Audio,
	Video,
	Other,
};

StreamType GetStreamType(const gchar* capsName) {
	if(g_str_has_prefix(capsName, "audio/"))
		return StreamType::Audio;
	else if(g_str_has_prefix(capsName, "video/"))
		return StreamType::Video;
	else
		return StreamType::Other;
}

void SetAudioProperties(Napi::Object* object, const GstAudioInfo& audioInfo) {
	if(audioInfo.channels)
		object->Set("channels", ToJsValue(object->Env(), audioInfo.channels));
	if(audioInfo.rate)
		object->Set("samplingRate", ToJsValue(object->Env(), audioInfo.rate));
	if(audioInfo.bpf)
		object->Set("sampleSize", ToJsValue(object->Env(), audioInfo.bpf));
}

void SetVideoProperties(Napi::Object* object, const GstVideoInfo& videoInfo) {
	object->Set("pixelFormat", ToJsValue(object->Env(), videoInfo.finfo->name));
	if(videoInfo.width)
		object->Set("width", ToJsValue(object->Env(), videoInfo.width));
	if(videoInfo.height)
		object->Set("height", ToJsValue(object->Env(), videoInfo.height));
}

}

struct JsPlayer::AppSinkData {
	AppSinkData(Napi::FunctionReference&& callback) :
		callback(std::move(callback)) {}
	AppSinkData(const AppSinkData& d) = delete;
	AppSinkData(AppSinkData&& d) :
		type(d.type),
		prerolled(d.prerolled),
		firstSample(d.firstSample),
		eos(d.eos),
		callback(std::move(d.callback)) {}

	std::optional<StreamType> type;
	std::string mediaType;
	std::optional<GstAudioInfo> audioInfo;
	std::optional<GstVideoInfo> videoInfo;

	bool prerolled = false;
	bool firstSample = true;
	bool eos = false;

	Napi::FunctionReference callback;
};

struct JsPlayer::PadProbeData {
	PadProbeData(Napi::FunctionReference&& callback) :
		callback(std::move(callback)) {}
	PadProbeData(const AppSinkData& d) = delete;
	PadProbeData(AppSinkData&& d) :
		callback(std::move(d.callback)) {}

	Napi::FunctionReference callback;
};

struct JsPlayer::AsyncEvent
{
	virtual ~AsyncEvent() {};

	virtual void forwardTo(JsPlayer*) const = 0;
};

struct JsPlayer::CapsChangedEvent : public JsPlayer::AsyncEvent
{
	CapsChangedEvent(GstPad* pad, GstCaps* caps) :
		pad(GST_PAD(gst_object_ref(pad))),
		caps(gst_caps_ref(caps)) {}
	CapsChangedEvent(CapsChangedEvent&) = delete;
	CapsChangedEvent(CapsChangedEvent&& source) :
		pad(source.pad),
		caps(source.caps)
	{
		source.pad = nullptr;
		source.caps = nullptr;
	}
	~CapsChangedEvent() override {
		if(caps)
			gst_caps_unref(caps);

		if(pad)
			gst_object_unref(pad);
	}

	void forwardTo(JsPlayer* player) const override {
		player->onCapsChanged(pad, caps);
	}

	GstPad* pad;
	GstCaps* caps;
};

struct JsPlayer::EosEvent : public JsPlayer::AsyncEvent
{
	void forwardTo(JsPlayer* player) const override {
		player->onEos();
	}
};

///////////////////////////////////////////////////////////////////////////////
Napi::FunctionReference JsPlayer::_jsConstructor;

Napi::Object JsPlayer::InitJsApi(Napi::Env env, Napi::Object exports)
{
	gst_init(0, 0);

	Napi::HandleScope scope(env);

	Napi::Function func =
		DefineClass(env, "JsPlayer", {
			InstanceValue("GST_STATE_VOID_PENDING", ToJsValue(env, GST_STATE_VOID_PENDING)),
			InstanceValue("GST_STATE_NULL", ToJsValue(env, GST_STATE_NULL)),
			InstanceValue("GST_STATE_READY", ToJsValue(env, GST_STATE_READY)),
			InstanceValue("GST_STATE_PAUSED", ToJsValue(env, GST_STATE_PAUSED)),
			InstanceValue("GST_STATE_PLAYING", ToJsValue(env, GST_STATE_PLAYING)),
			InstanceValue("AppSinkSetup", ToJsValue(env, Setup)),
			InstanceValue("AppSinkNewPreroll", ToJsValue(env, NewPreroll)),
			InstanceValue("AppSinkNewSample", ToJsValue(env, NewSample)),
			InstanceValue("AppSinkEos", ToJsValue(env, Eos)),
			CLASS_METHOD("parseLaunch", &JsPlayer::parseLaunch),
			CLASS_METHOD("addAppSinkCallback", &JsPlayer::addAppSinkCallback),
			CLASS_METHOD("addCapsProbe", &JsPlayer::addCapsProbe),
			CLASS_METHOD("setState", &JsPlayer::setState),
			CLASS_METHOD("sendEos", &JsPlayer::sendEos),
		}
	);

	_jsConstructor = Napi::Persistent(func);
	_jsConstructor.SuppressDestruct();

	exports.Set("JsPlayer", func);

	return exports;
}

JsPlayer::JsPlayer(const Napi::CallbackInfo& info) :
	Napi::ObjectWrap<JsPlayer>(info),
	_pipeline(nullptr)
{
	if(info.Length() > 0) {
		Napi::Value arg0 = info[0];
		if(arg0.IsFunction())
			_eosCallback = Napi::Persistent(arg0.As<Napi::Function>());
	}

	uv_loop_t* loop = uv_default_loop();

	_queueAsync = new uv_async_t;
	uv_async_init(loop, _queueAsync,
		[] (uv_async_t* handle) {
			if(handle->data)
				reinterpret_cast<JsPlayer*>(handle->data)->handleQueue();
		}
	);
	_queueAsync->data = this;

	_async = new uv_async_t;
	uv_async_init(loop, _async,
		[] (uv_async_t* handle) {
			if(handle->data)
				reinterpret_cast<JsPlayer*>(handle->data)->handleAsync();
		}
	);
	_async->data = this;
}

JsPlayer::~JsPlayer()
{
	close();
}

void JsPlayer::cleanup()
{
	if(_pipeline) {
		setState(GST_STATE_NULL);

		for(const auto& pair: _padsProbes) {
			gst_object_unref(pair.first);
		}
		_padsProbes.clear();

		for(const auto& pair: _appSinks) {
			gst_object_unref(pair.first);
		}
		_appSinks.clear();

		gst_object_unref(_pipeline);
		_pipeline = nullptr;
	}
}

void JsPlayer::close()
{
	cleanup();

	_queueAsync->data = nullptr;
	uv_close(
		reinterpret_cast<uv_handle_t*>(_queueAsync),
		[] (uv_handle_s* handle) {
			delete reinterpret_cast<uv_async_t*>(handle);
		});
	_queueAsync = nullptr;

	_async->data = nullptr;
	uv_close(
		reinterpret_cast<uv_handle_t*>(_async),
		[] (uv_handle_s* handle) {
			delete reinterpret_cast<uv_async_t*>(handle);
		});
	_async = nullptr;
}

void JsPlayer::handleAsync()
{
	Napi::HandleScope scope(Env());

	for(auto& pair: _appSinks) {
		GstAppSink* appSink = pair.first;
		AppSinkData& appSinkData = pair.second;
		if(appSinkData.eos) continue;

		if(!appSinkData.prerolled) {
			if(g_autoptr(GstSample) prerollSample = gst_app_sink_try_pull_preroll(appSink, 0)) {
				onSample(&appSinkData, prerollSample, true);
				appSinkData.prerolled = true;
			}
		}

		while(g_autoptr(GstSample) sample = gst_app_sink_try_pull_sample(appSink, 0)) {
			onSample(&appSinkData, sample, false);
		}

		appSinkData.eos = gst_app_sink_is_eos(appSink);
		if(appSinkData.eos) {
			onEos(appSink);
		}
	}
}

void JsPlayer::onSetup(JsPlayer::AppSinkData* sinkData)
{
	assert(sinkData && sinkData->type.has_value());
	if(!sinkData || !sinkData->type.has_value() || sinkData->callback.IsEmpty())
		return;

	switch(sinkData->type.value()) {
		case StreamType::Audio: {
			assert(sinkData->audioInfo.has_value());
			if(!sinkData->audioInfo.has_value())
				break;

			const GstAudioInfo& audioInfo = sinkData->audioInfo.value();

			Napi::Object propertiesObject = Napi::Object::New(Env());
			SetAudioProperties(&propertiesObject, audioInfo);

			sinkData->callback.Call({
				ToJsValue(Env(), Setup),
				ToJsValue(Env(), sinkData->mediaType),
				propertiesObject,
			});
			break;
		}
		case StreamType::Video: {
			assert(sinkData->videoInfo.has_value());
			if(!sinkData->videoInfo.has_value())
				break;

			const GstVideoInfo& videoInfo = sinkData->videoInfo.value();

			Napi::Object propertiesObject = Napi::Object::New(Env());
			SetVideoProperties(&propertiesObject, videoInfo);

			sinkData->callback.Call({
				ToJsValue(Env(), Setup),
				ToJsValue(Env(), sinkData->mediaType),
				propertiesObject,
			});
			break;
		}
		case StreamType::Other: {
			Napi::Object propertiesObject = Napi::Object::New(Env());

			sinkData->callback.Call({
				ToJsValue(Env(), Setup),
				ToJsValue(Env(), sinkData->mediaType),
				propertiesObject,
			});
			break;
		}
	}
}

void JsPlayer::onAudioSample(
	AppSinkData* sinkData,
	GstSample* sample,
	bool preroll)
{
	if(!sinkData || sinkData->callback.IsEmpty())
		return;

	GstBuffer* buffer = gst_sample_get_buffer(sample);
	if(!buffer)
		return;

	GstMapInfo mapInfo;
	if(gst_buffer_map(buffer, &mapInfo, GST_MAP_READ)) {
		Napi::Buffer<unsigned char> sample =
			Napi::Buffer<unsigned char>::Copy(Env(), mapInfo.data, mapInfo.size);
		Napi::Object sampleObject(Env(), sample);

		sinkData->callback.Call({
			ToJsValue(Env(), (preroll ? NewPreroll : NewSample)),
			sampleObject,
		});
		gst_buffer_unmap(buffer, &mapInfo);
	}
}

void JsPlayer::onVideoSample(
	AppSinkData* sinkData,
	GstSample* sample,
	bool preroll)
{
	if(!sinkData || !sinkData->videoInfo || sinkData->callback.IsEmpty())
		return;

	const GstVideoInfo& videoInfo = sinkData->videoInfo.value();

	GstBuffer* buffer = gst_sample_get_buffer(sample);
	if(!buffer)
		return;

	GstMapInfo mapInfo;
	if(gst_buffer_map(buffer, &mapInfo, GST_MAP_READ)) {
		Napi::Buffer<unsigned char> sample =
			Napi::Buffer<unsigned char>::Copy(Env(), mapInfo.data, mapInfo.size);
		Napi::Object sampleObject(Env(), sample);
		sampleObject.Set("width", ToJsValue(Env(), videoInfo.width));
		sampleObject.Set("height", ToJsValue(Env(), videoInfo.height));

		if(videoInfo.finfo->n_planes) {
			Napi::Array planesArray = Napi::Array::New(Env(), videoInfo.finfo->n_planes);
			for(guint p = 0; p < videoInfo.finfo->n_planes; ++p)
				planesArray.Set(p, ToJsValue(Env(), static_cast<int>(videoInfo.offset[p])));

			sampleObject.Set("planes", planesArray);
		}

		sinkData->callback.Call({
			ToJsValue(Env(), (preroll ? NewPreroll : NewSample)),
			sampleObject,
		});
		gst_buffer_unmap(buffer, &mapInfo);
	}
}

void JsPlayer::onOtherSample(
	AppSinkData* sinkData,
	GstSample* sample,
	bool preroll)
{
	if(!sinkData || sinkData->callback.IsEmpty())
		return;

	GstBuffer* buffer = gst_sample_get_buffer(sample);
	if(!buffer)
		return;

	GstMapInfo mapInfo;
	if(gst_buffer_map(buffer, &mapInfo, GST_MAP_READ)) {
		Napi::Buffer<unsigned char> sample =
			Napi::Buffer<unsigned char>::Copy(Env(), mapInfo.data, mapInfo.size);
		Napi::Object sampleObject(Env(), sample);

		sinkData->callback.Call({
			ToJsValue(Env(), (preroll ? NewPreroll : NewSample)),
			sampleObject,
		});
		gst_buffer_unmap(buffer, &mapInfo);
	}
}

void JsPlayer::onSample(
	AppSinkData* sinkData,
	GstSample* sample,
	bool preroll)
{
	if(!sample)
		return;

	if(!sinkData->type) {
		const gchar* capsName = "";
		GstCaps* caps = gst_sample_get_caps(sample);
		if(caps) {
			GstStructure* capsStructure = gst_caps_get_structure(caps, 0);
			capsName = gst_structure_get_name(capsStructure);
			sinkData->mediaType = capsName;
		}

		sinkData->type = GetStreamType(capsName);
		switch(*sinkData->type) {
			case StreamType::Audio: {
				GstAudioInfo audioInfo;
				if(gst_audio_info_from_caps(&audioInfo, caps)) {
					sinkData->audioInfo.emplace(audioInfo);
				}
				break;
			}
			case StreamType::Video: {
				GstVideoInfo videoInfo;
				if(gst_video_info_from_caps(&videoInfo, caps)) {
					sinkData->videoInfo.emplace(videoInfo);
				}
				break;
			}
			case StreamType::Other:
				break;
		}
	}

	if(sinkData->firstSample) {
		onSetup(sinkData);
		sinkData->firstSample = false;
	}

	switch(*sinkData->type) {
	case StreamType::Audio:
		onAudioSample(sinkData, sample, preroll);
		break;
	case StreamType::Video:
		onVideoSample(sinkData, sample, preroll);
		break;
	case StreamType::Other:
		onOtherSample(sinkData, sample, preroll);
		break;
	}
}

void JsPlayer::onEos(GstAppSink* appSink)
{
	if(!appSink)
		return;

	auto it = _appSinks.find(appSink);
	if(_appSinks.end() == it)
		return;

	if(it->second.callback.IsEmpty())
		return;

	it->second.callback.Call({
		ToJsValue(Env(), Eos),
	});
}

void JsPlayer::onCapsChanged(GstPad* pad, GstCaps* caps)
{
	if(!pad || !caps)
		return;

	auto it = _padsProbes.find(pad);
	if(_padsProbes.end() == it)
		return;

	if(it->second.callback.IsEmpty())
		return;

	Napi::HandleScope scope(Env());

	GstStructure* capsStructure = gst_caps_get_structure(caps, 0);
	const gchar* capsName = gst_structure_get_name(capsStructure);
	const StreamType type = GetStreamType(capsName);
	switch(type) {
		case StreamType::Audio: {
			GstAudioInfo audioInfo;
			if(gst_audio_info_from_caps(&audioInfo, caps)) {
				Napi::Object propertiesObject = Napi::Object::New(Env());
				SetAudioProperties(&propertiesObject, audioInfo);

				it->second.callback.Call({
					ToJsValue(Env(), capsName),
					propertiesObject,
				});
			}
			break;
		}
		case StreamType::Video: {
			GstVideoInfo videoInfo;
			if(gst_video_info_from_caps(&videoInfo, caps)) {
				Napi::Object propertiesObject = Napi::Object::New(Env());
				SetVideoProperties(&propertiesObject, videoInfo);

				it->second.callback.Call({
					ToJsValue(Env(), capsName),
					propertiesObject,
				});
			}
			break;
		}
		case StreamType::Other: {
			Napi::Object propertiesObject = Napi::Object::New(Env());

			it->second.callback.Call({
				ToJsValue(Env(), capsName),
				propertiesObject,
			});
			break;
		}
	}
}

void JsPlayer::onEos()
{
	if(_eosCallback.IsEmpty())
		return;

	handleAsync(); // just in case to don't miss queued samples

	Napi::HandleScope scope(Env());
	_eosCallback.Call({});
}

void JsPlayer::handleQueue()
{
	_queueGuard.lock();
	std::deque<std::unique_ptr<AsyncEvent>> tmpQueue = std::move(_queue);
	_queueGuard.unlock();

	for(const std::unique_ptr<AsyncEvent>& event: tmpQueue) {
		event->forwardTo(this);
	}
}

bool JsPlayer::parseLaunch(const std::string& pipelineDescription)
{
	cleanup();

	GError* error = nullptr;
	_pipeline = gst_parse_launch(pipelineDescription.c_str(), &error);

	if(!_pipeline)
		return false;

	GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline));
	gst_bus_set_sync_handler(
		bus,
		[] (GstBus*, GstMessage* message, gpointer userData) -> GstBusSyncReply {
			if(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
				JsPlayer* player = static_cast<JsPlayer*>(userData);

				std::lock_guard(player->_queueGuard);
				player->_queue.emplace_back(std::make_unique<EosEvent>());
				uv_async_send(player->_queueAsync);
			}

			return GST_BUS_PASS;
		},
		this,
		nullptr);
	gst_object_unref(bus);

	return true;
}

bool JsPlayer::addAppSinkCallback(
	const std::string& appSinkName,
	const Napi::Function& callback)
{
	if(!_pipeline || appSinkName.empty())
		return false;

	g_autoptr(GstElement) sink = gst_bin_get_by_name(GST_BIN(_pipeline), appSinkName.c_str());
	if(!sink)
		return false;

	GstAppSink* appSink = GST_APP_SINK_CAST(sink);
	if(!appSink)
		return false;

	if(callback.IsEmpty())
		return false;

	auto it = _appSinks.find(appSink);
	if(_appSinks.end() == it) {
		GstAppSinkCallbacks callbacks = {
			[] (GstAppSink*, gpointer userData) {
				uv_async_send(static_cast<JsPlayer*>(userData)->_async);
			},
			[] (GstAppSink*, gpointer userData) -> GstFlowReturn {
				uv_async_send(static_cast<JsPlayer*>(userData)->_async);
				return GST_FLOW_OK;
			},
			[] (GstAppSink*, gpointer userData) -> GstFlowReturn {
				uv_async_send(static_cast<JsPlayer*>(userData)->_async);
				return GST_FLOW_OK;
			} };
		gst_app_sink_set_callbacks(appSink, &callbacks, this, nullptr);
		_appSinks.emplace(appSink, Napi::Persistent(callback));
		sink = nullptr;
	} else {
		it->second.callback = std::move(Napi::Persistent(callback));
	}

	return true;
}

bool JsPlayer::addCapsProbe(
	const std::string& elementName,
	const std::string& padName,
	const Napi::Function& callback)
{
	if(!_pipeline || elementName.empty() || padName.empty())
		return false;

	g_autoptr(GstElement) element = gst_bin_get_by_name(GST_BIN(_pipeline), elementName.c_str());
	if(!element)
		return false;

	g_autoptr(GstPad) pad = gst_element_get_static_pad(element, padName.c_str());
	if(!pad)
		return false;

	if(_padsProbes.find(pad) != _padsProbes.end())
		return false;

	const gulong probeId = gst_pad_add_probe(
			pad,
			GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
			[] (GstPad* pad, GstPadProbeInfo* info, gpointer userData) -> GstPadProbeReturn {
					if(GST_EVENT_TYPE(GST_PAD_PROBE_INFO_DATA(info)) != GST_EVENT_CAPS)
							return GST_PAD_PROBE_OK;

					GstEvent* event = GST_EVENT_CAST(GST_PAD_PROBE_INFO_DATA(info));
					g_autoptr(GstCaps) caps = nullptr;
					gst_event_parse_caps(event, &caps);

					JsPlayer* player = static_cast<JsPlayer*>(userData);

					std::lock_guard(player->_queueGuard);
					player->_queue.emplace_back(std::make_unique<CapsChangedEvent>(pad, caps));
					uv_async_send(player->_queueAsync);

					return GST_PAD_PROBE_OK;
			},
			this,
			nullptr);
	if(!probeId)
		return false;

	_padsProbes.emplace(pad, Napi::Persistent(callback));
	pad = nullptr;

	return true;
}

void JsPlayer::setState(unsigned state)
{
	if(_pipeline)
		gst_element_set_state(_pipeline, static_cast<GstState>(state));
}

void JsPlayer::sendEos()
{
	if(_pipeline)
		gst_element_send_event(_pipeline, gst_event_new_eos());
}
