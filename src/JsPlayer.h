#pragma once

#include <memory>
#include <deque>
#include <set>
#include <map>
#include <mutex>
#include <atomic>

#include <uv.h>

#include <NapiHelpers.h>

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>
#include <gst/app/gstappsink.h>


class JsPlayer : public Napi::ObjectWrap<JsPlayer>
{
	static Napi::FunctionReference _jsConstructor;

public:
	static Napi::Object InitJsApi(Napi::Env env, Napi::Object exports);

	JsPlayer(const Napi::CallbackInfo&);
	~JsPlayer();

private:
	enum AppSinkEvent {
		Setup = 0,
		NewPreroll,
		NewSample,
		Eos,
	};

	void close();

	bool parseLaunch(const std::string& pipelineDescription);

	bool addAppSinkCallback(
		const std::string& appSinkName,
		const Napi::Function& callback);

	bool addCapsProbe(
		const std::string& elementName,
		const std::string& padName,
		const Napi::Function& callback);

	void setState(unsigned state);

	void sendEos();

private:
	struct AppSinkData;

	void handleAsync();

	void onSetup(AppSinkData*);

	void onSample(
		AppSinkData*,
		GstSample*,
		bool preroll);
	void onVideoSample(
		AppSinkData*,
		GstSample*,
		bool preroll);
	void onAudioSample(
		AppSinkData*,
		GstSample*,
		bool preroll);
	void onOtherSample(
		AppSinkData*,
		GstSample*,
		bool preroll);

	void onEos(GstAppSink*);

	void handleQueue();
	void onCapsChanged(GstPad* pad, GstCaps* caps);

	void cleanup();

private:
	Napi::FunctionReference _eosCallback;

	GstElement* _pipeline;

	struct AppSinkData;
	std::map<GstAppSink*, AppSinkData> _appSinks;

	struct PadProbeData;
	std::map<GstPad*, PadProbeData> _padsProbes;

	uv_async_t* _queueAsync = nullptr;
	struct AsyncEvent;
	struct CapsChanged;
	std::mutex _queueGuard;
	std::deque<std::unique_ptr<AsyncEvent>> _queue;

	uv_async_t* _async = nullptr;
};
