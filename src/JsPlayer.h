#pragma once

#include <memory>
#include <deque>
#include <set>
#include <map>
#include <mutex>

#include <v8.h>
#include <node.h>
#include <node_object_wrap.h>
#include <uv.h>

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>

class JsPlayer : public node::ObjectWrap
{
public:
    static void initJsApi( const v8::Handle<v8::Object>& exports );

private:
    static void jsCreate( const v8::FunctionCallbackInfo<v8::Value>& args );
    static void closeAll();

    JsPlayer( v8::Local<v8::Object>& thisObject );
    ~JsPlayer();

    enum AppSinkEvent {
        Setup = 0,
        NewPreroll,
        NewSample,
        Eos,
    };

    void close();

    bool parseLaunch( const std::string& pipelineDescription );

    bool setAppSinkCallback( const std::string& appSinkName, v8::Local<v8::Value> value );

    void setState( unsigned state );

private:
    static void onEosProxy( GstAppSink* appSink, gpointer userData );
    static GstFlowReturn onNewPrerollProxy( GstAppSink *appsink, gpointer userData );
    static GstFlowReturn onNewSampleProxy( GstAppSink *appsink, gpointer userData );

    struct AsyncData;
    struct AppSinkEventData;

    void handleAsync();

    void onSetup( GstAppSink*, const GstVideoInfo& );
    void onNewPreroll( GstAppSink* );
    void onNewSample( GstAppSink* );
    void onEos( GstAppSink* );

    void onSample( GstAppSink*, GstSample*, bool preroll );

    void callCallback( GstAppSink* appSink, AppSinkEvent event,
                       std::initializer_list<v8::Local<v8::Value> > list = std::initializer_list<v8::Local<v8::Value> >() );

private:
    static v8::Persistent<v8::Function> _jsConstructor;
    static std::set<JsPlayer*> _instances;

    GstElement* _pipeline;

    struct AppSinkData {
        AppSinkData() {
            waitingSample.test_and_set();
        }
        AppSinkData( const AppSinkData& d ) = delete;
        AppSinkData( AppSinkData&& d ) : callback( std::move( d.callback ) ) {
            waitingSample.test_and_set();
        }

        std::atomic_flag waitingSample;
        v8::UniquePersistent<v8::Function> callback;
    };

    std::map<GstAppSink*, AppSinkData > _appSinks;
    bool _firstSample;

    uv_async_t _async;
    std::mutex _asyncDataGuard;
    std::deque<std::unique_ptr<AsyncData> > _asyncData;
};
