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

	struct AsyncData;
	struct AppSinkEventData;

	void handleAsync();

	void onSetup(GstAppSink*, const GstVideoInfo&);
	void onNewPreroll(GstAppSink*);
	void onNewSample(GstAppSink*);
	void onEos(GstAppSink*);

	void onSample(GstAppSink*, GstSample*, bool preroll);

private:
	GstElement* _pipeline;

	struct AppSinkData {
		AppSinkData() {
			waitingSample.test_and_set();
		}
		AppSinkData(const AppSinkData& d) = delete;
		AppSinkData(AppSinkData&& d) : callback(std::move(d.callback)) {
			waitingSample.test_and_set();
		}

		std::atomic_flag waitingSample;
		Napi::FunctionReference callback;
	};

	std::map<GstAppSink*, AppSinkData> _appSinks;
	bool _firstSample;

	uv_async_t _async;
	std::mutex _asyncDataGuard;
	std::deque<std::unique_ptr<AsyncData>> _asyncData;
};
