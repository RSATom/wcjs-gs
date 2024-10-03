#include "JsPlayer.h"

#include <cassert>
#include <optional>

#include <glib.h>


namespace {

enum class SinkType
{
	Audio,
	Video,
	Other,
};

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

	std::optional<SinkType> type;
	std::string mediaType;
	std::optional<GstAudioInfo> audioInfo;
	std::optional<GstVideoInfo> videoInfo;

	bool prerolled = false;
	bool firstSample = true;
	bool eos = false;

	Napi::FunctionReference callback;
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

		_appSinks.clear();
		gst_object_unref(_pipeline);
		_pipeline = nullptr;
	}
}

void JsPlayer::close()
{
	cleanup();

	_async->data = nullptr;
	uv_close(
		reinterpret_cast<uv_handle_t*>(_async),
		[] (uv_handle_s* handle) {
			delete static_cast<uv_handle_t*>(handle);
		});
	_async = nullptr;
}

void JsPlayer::handleAsync()
{
	Napi::HandleScope scope(Env());

	decltype(_appSinks)::size_type eosSinks = 0;
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
			++eosSinks;
		}
	}

  if(eosSinks == _appSinks.size() && !_eosCallback.IsEmpty()) {
		_eosCallback.Call({});
	}
}

GstBusSyncReply JsPlayer::onBusMessageProxy(GstBus*, GstMessage* message, gpointer userData)
{
	JsPlayer* player = static_cast<JsPlayer*>(userData);

	switch(GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_EOS:
		uv_async_send(player->_async);
		break;
	}

	return GST_BUS_PASS;
}

GstFlowReturn JsPlayer::onNewPrerollProxy(GstAppSink *appSink, gpointer userData)
{
	JsPlayer* player = static_cast<JsPlayer*>(userData);

	uv_async_send(player->_async);

	return GST_FLOW_OK;
}

GstFlowReturn JsPlayer::onNewSampleProxy(GstAppSink *appSink, gpointer userData)
{
	JsPlayer* player = static_cast<JsPlayer*>(userData);

	uv_async_send(player->_async);

	return GST_FLOW_OK;
}

void JsPlayer::onEosProxy(GstAppSink* appSink, gpointer userData)
{
	JsPlayer* player = static_cast<JsPlayer*>(userData);

	uv_async_send(player->_async);
}

void JsPlayer::onSetup(JsPlayer::AppSinkData* sinkData)
{
	assert(sinkData && sinkData->type.has_value());
	if(!sinkData || !sinkData->type.has_value() || sinkData->callback.IsEmpty())
		return;

	switch(sinkData->type.value()) {
		case SinkType::Audio: {
			assert(sinkData->audioInfo.has_value());
			if(!sinkData->audioInfo.has_value())
				break;

			const GstAudioInfo& audioInfo = sinkData->audioInfo.value();

			Napi::Object propertiesObject = Napi::Object::New(Env());
			propertiesObject.Set("channels", ToJsValue(Env(), audioInfo.channels));
			propertiesObject.Set("samplingRate", ToJsValue(Env(), audioInfo.rate));
			propertiesObject.Set("sampleSize", ToJsValue(Env(), audioInfo.bpf));

			sinkData->callback.Call({
				ToJsValue(Env(), Setup),
				ToJsValue(Env(), sinkData->mediaType),
				propertiesObject,
			});
			break;
		}
		case SinkType::Video: {
			assert(sinkData->videoInfo.has_value());
			if(!sinkData->videoInfo.has_value())
				break;

			const GstVideoInfo& videoInfo = sinkData->videoInfo.value();

			Napi::Object propertiesObject = Napi::Object::New(Env());
			propertiesObject.Set("pixelFormat", ToJsValue(Env(), videoInfo.finfo->name));
			propertiesObject.Set("width", ToJsValue(Env(), videoInfo.width));
			propertiesObject.Set("height", ToJsValue(Env(), videoInfo.height));

			sinkData->callback.Call({
				ToJsValue(Env(), Setup),
				ToJsValue(Env(), sinkData->mediaType),
				propertiesObject,
			});
			break;
		}
		case SinkType::Other: {
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

		if(g_str_has_prefix(capsName, "audio/")) {
			sinkData->type = SinkType::Audio;
			GstAudioInfo audioInfo;
			if(gst_audio_info_from_caps(&audioInfo, caps)) {
				sinkData->audioInfo.emplace(audioInfo);
			}
		} else if(g_str_has_prefix(capsName, "video/")) {
			sinkData->type = SinkType::Video;
			GstVideoInfo videoInfo;
			if(gst_video_info_from_caps(&videoInfo, caps)) {
				sinkData->videoInfo.emplace(videoInfo);
			}
		} else {
			sinkData->type = SinkType::Other;
		}
	}

	if(sinkData->firstSample) {
		onSetup(sinkData);
		sinkData->firstSample = false;
	}

	switch(*sinkData->type) {
	case SinkType::Audio:
		onAudioSample(sinkData, sample, preroll);
		break;
	case SinkType::Video:
		onVideoSample(sinkData, sample, preroll);
		break;
	case SinkType::Other:
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

bool JsPlayer::parseLaunch(const std::string& pipelineDescription)
{
	cleanup();

	GError* error = nullptr;
	_pipeline = gst_parse_launch(pipelineDescription.c_str(), &error);

	if(!_pipeline)
		return false;

	GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline));
	gst_bus_set_sync_handler(bus, onBusMessageProxy, this, nullptr);
	gst_object_unref(bus);

	return true;
}

bool JsPlayer::addAppSinkCallback(
	const std::string& appSinkName,
	const Napi::Function& callback)
{
	if(!_pipeline || appSinkName.empty())
		return false;

	GstElement* sink = gst_bin_get_by_name(GST_BIN(_pipeline), appSinkName.c_str());
	if(!sink)
		return false;

	GstAppSink* appSink = GST_APP_SINK_CAST(sink);
	if(!appSink)
		return false;

	if(callback.IsEmpty())
		return false;

	auto it = _appSinks.find(appSink);
	if(_appSinks.end() == it) {
		gst_app_sink_set_drop(appSink, true);
		if(
			!gst_app_sink_get_max_bytes(appSink) &&
			!gst_app_sink_get_max_buffers(appSink) &&
			!gst_app_sink_get_max_time(appSink)
		) {
			gst_app_sink_set_max_buffers(appSink, 1);
		}
		GstAppSinkCallbacks callbacks = { onEosProxy, onNewPrerollProxy, onNewSampleProxy };
		gst_app_sink_set_callbacks(appSink, &callbacks, this, nullptr);
		_appSinks.emplace(appSink, Napi::Persistent(callback));
	} else {
		it->second.callback = std::move(Napi::Persistent(callback));
	}

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
