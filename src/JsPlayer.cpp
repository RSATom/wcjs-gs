#include "JsPlayer.h"


///////////////////////////////////////////////////////////////////////////////
struct JsPlayer::AsyncData
{
	virtual ~AsyncData() {};

	virtual void process(JsPlayer*) = 0;
};

///////////////////////////////////////////////////////////////////////////////
struct JsPlayer::AppSinkEventData : public JsPlayer::AsyncData
{
	AppSinkEventData(GstAppSink* appSink, JsPlayer::AppSinkEvent event) :
		appSink(appSink), event(event) {}

	void process(JsPlayer*);

	GstAppSink* appSink;
	const JsPlayer::AppSinkEvent event;
};

void JsPlayer::AppSinkEventData::process(JsPlayer* player)
{
	switch(event) {
	case JsPlayer::AppSinkEvent::NewPreroll:
		player->onNewPreroll(appSink);
		break;
	case JsPlayer::AppSinkEvent::NewSample:
		player->onNewSample(appSink);
		break;
	case JsPlayer::AppSinkEvent::Eos:
		player->onEos(appSink);
		break;
	default:
		break;
	}
}

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
		}
	);

	_jsConstructor = Napi::Persistent(func);
	_jsConstructor.SuppressDestruct();

	exports.Set("JsPlayer", func);

	return exports;
}

JsPlayer::JsPlayer(const Napi::CallbackInfo& info) :
	Napi::ObjectWrap<JsPlayer>(info),
	_pipeline(nullptr), _firstSample(true)
{
	uv_loop_t* loop = uv_default_loop();

	uv_async_init(loop, &_async,
		[] (uv_async_t* handle) {
			if(handle->data)
				reinterpret_cast<JsPlayer*>(handle->data)->handleAsync();
		}
	);
	_async.data = this;
}

JsPlayer::~JsPlayer()
{
	close();
}

void JsPlayer::close()
{
	_async.data = nullptr;
	uv_close(reinterpret_cast<uv_handle_t*>(&_async), 0);
}

void JsPlayer::handleAsync()
{
	while(!_asyncData.empty()) {
		std::deque<std::unique_ptr<AsyncData> > tmpData;
		_asyncDataGuard.lock();
		_asyncData.swap(tmpData);
		_asyncDataGuard.unlock();

		for(const auto& i: tmpData) {
			i->process(this);
		}
	}
}

GstFlowReturn JsPlayer::onNewPrerollProxy(GstAppSink *appSink, gpointer userData)
{
	JsPlayer* player = static_cast<JsPlayer*>(userData);

	player->_asyncDataGuard.lock();
	player->_asyncData.emplace_back(new AppSinkEventData(appSink, NewPreroll));
	player->_asyncDataGuard.unlock();
	uv_async_send(&player->_async);

	return GST_FLOW_OK;
}

GstFlowReturn JsPlayer::onNewSampleProxy(GstAppSink *appSink, gpointer userData)
{
	JsPlayer* player = static_cast<JsPlayer*>(userData);

	player->_appSinks[appSink].waitingSample.clear();
	player->_asyncDataGuard.lock();
	player->_asyncData.emplace_back(new AppSinkEventData(appSink, NewSample));
	player->_asyncDataGuard.unlock();
	uv_async_send(&player->_async);

	return GST_FLOW_OK;
}

void JsPlayer::onEosProxy(GstAppSink* appSink, gpointer userData)
{
	JsPlayer* player = static_cast<JsPlayer*>(userData);

	player->_asyncDataGuard.lock();
	player->_asyncData.emplace_back(new AppSinkEventData(appSink, Eos));
	player->_asyncDataGuard.unlock();
	uv_async_send(&player->_async);
}

void JsPlayer::onSetup(GstAppSink* appSink, const GstVideoInfo& videoInfo)
{
	if(!appSink)
		return;

	_firstSample = false;

	auto it = _appSinks.find(appSink);
	if(_appSinks.end() == it)
		return;

	if(it->second.callback.IsEmpty())
		return;

	it->second.callback.Call({
		ToJsValue(Env(), Setup),
		ToJsValue(Env(), videoInfo.finfo->name),
		ToJsValue(Env(), videoInfo.width),
		ToJsValue(Env(), videoInfo.height),
		ToJsValue(Env(), videoInfo.finfo->format)
	});
}

void JsPlayer::onNewPreroll(GstAppSink* appSink)
{
	GstSample* sample = gst_app_sink_pull_preroll(appSink);

	onSample(appSink, sample, true);

	gst_sample_unref(sample);
}

void JsPlayer::onNewSample(GstAppSink* appSink)
{
	if(_appSinks[appSink].waitingSample.test_and_set())
		return;

	GstSample* sample = gst_app_sink_pull_sample(appSink);

	_appSinks[appSink].waitingSample.test_and_set();

	onSample(appSink, sample, false);

	gst_sample_unref(sample);
}

void JsPlayer::onSample(GstAppSink* appSink, GstSample* sample, bool preroll)
{
	if(!sample)
		return;

	GstVideoInfo videoInfo;

	GstCaps* caps = gst_sample_get_caps(sample);
	if(!caps || !gst_video_info_from_caps(&videoInfo, caps))
		return;

	if(_firstSample)
		onSetup(appSink, videoInfo);

	GstBuffer* buffer = gst_sample_get_buffer(sample);

	if(!buffer)
		return;

	auto it = _appSinks.find(appSink);
	if(_appSinks.end() == it)
		return;

	if(it->second.callback.IsEmpty())
		return;

	GstMapInfo mapInfo;
	if(gst_buffer_map(buffer, &mapInfo, GST_MAP_READ)) {
		Napi::Buffer<unsigned char> frame =
			Napi::Buffer<unsigned char>::Copy(Env(), mapInfo.data, mapInfo.size);
		Napi::Object frameObject(Env(), frame);
		frameObject.Set("width", ToJsValue(Env(), videoInfo.width));
		frameObject.Set("height", ToJsValue(Env(), videoInfo.height));

		Napi::Array planesArray = Napi::Array::New(Env(), videoInfo.finfo->n_planes);
		for(guint p = 0; p < videoInfo.finfo->n_planes; ++p)
			planesArray.Set(p, ToJsValue(Env(), static_cast<int>(videoInfo.offset[p])));

		frameObject.Set("planes", planesArray);

		it->second.callback.Call({
			ToJsValue(Env(), (preroll ? NewPreroll : NewSample)),
			frameObject,
		});
		gst_buffer_unmap(buffer, &mapInfo);
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
	if(_pipeline) {
		_appSinks.clear();
		gst_object_unref(_pipeline);
		_pipeline = nullptr;
		_firstSample = true;
	}

	GError* error = nullptr;
	_pipeline = gst_parse_launch(pipelineDescription.c_str(), &error);

	return (nullptr != _pipeline);
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
		return appSink;

	if(callback.IsEmpty())
		return false;

	auto it = _appSinks.find(appSink);
	if(_appSinks.end() == it) {
		gst_app_sink_set_drop(appSink, true);
		gst_app_sink_set_max_buffers(appSink, 1);
		GstAppSinkCallbacks callbacks = { onEosProxy, onNewPrerollProxy, onNewSampleProxy };
		gst_app_sink_set_callbacks(appSink, &callbacks, this, nullptr);
		it = _appSinks.emplace(appSink, AppSinkData()).first;
	}

	AppSinkData& sinkData = it->second;
	sinkData.callback = Napi::Persistent(callback);
	sinkData.waitingSample.test_and_set();

	return true;
}

void JsPlayer::setState(unsigned state)
{
	if(_pipeline)
		gst_element_set_state(_pipeline, static_cast<GstState>(state));
}
