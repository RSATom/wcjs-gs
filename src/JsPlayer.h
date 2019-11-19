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

	bool addAppSinkCallback(const std::string& appSinkName, const Napi::Function& callback);

	void setState(unsigned state);

private:
	static void onEosProxy(GstAppSink* appSink, gpointer userData);
	static GstFlowReturn onNewPrerollProxy(GstAppSink *appsink, gpointer userData);
	static GstFlowReturn onNewSampleProxy(GstAppSink *appsink, gpointer userData);

	struct AppSinkData;
	struct AsyncData;
	struct AppSinkEventData;

	void handleAsync();

	void onSetup(
		AppSinkData*,
		const GstVideoInfo&);
	void onSetup(
		AppSinkData*,
		const GstAudioInfo&);
	void onSetup(
		AppSinkData*,
		const gchar* type,
		const gchar* format);
	void onNewPreroll(GstAppSink*);
	void onNewSample(GstAppSink*);
	void onEos(GstAppSink*);

	void onSample(GstAppSink*, GstSample*, bool preroll);
	void onVideoSample(
		AppSinkData*,
		GstSample*,
		bool preroll,
		GstCaps*,
		const gchar* format);
	void onAudioSample(
		AppSinkData*,
		GstSample*,
		bool preroll,
		GstCaps*,
		const gchar* format);
	void onOtherSample(
		AppSinkData*,
		GstSample*,
		bool preroll,
		GstCaps*,
		const gchar* capsName);

	void cleanup();

private:
	GstElement* _pipeline;

	struct AppSinkData {
		AppSinkData() :
			firstSample(true)
			{ waitingSample.test_and_set(); }
		AppSinkData(const AppSinkData& d) = delete;
		AppSinkData(AppSinkData&& d) :
			firstSample(true), callback(std::move(d.callback))
			{ waitingSample.test_and_set(); }

		bool firstSample;
		std::atomic_flag waitingSample;
		Napi::FunctionReference callback;
	};

	std::map<GstAppSink*, AppSinkData> _appSinks;

	uv_async_t _async;
	std::mutex _asyncDataGuard;
	std::deque<std::unique_ptr<AsyncData>> _asyncData;
};
