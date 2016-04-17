#include "JsPlayer.h"

#include <string.h>

#include <node_buffer.h>

#include <nah/NodeHelpers.h>

#if NODE_MAJOR_VERSION > 3 || \
    ( NODE_MAJOR_VERSION == 3 && NODE_MINOR_VERSION > 0 ) || \
    ( NODE_MAJOR_VERSION == 3 && NODE_MINOR_VERSION == 0 && NODE_BUILD_NUMBER >= 0 )

#define USE_MAYBE_LOCAL 1

#endif


///////////////////////////////////////////////////////////////////////////////
struct JsPlayer::AsyncData
{
    virtual void process( JsPlayer* ) = 0;
};

///////////////////////////////////////////////////////////////////////////////
struct JsPlayer::AppSinkEventData : public JsPlayer::AsyncData
{
    AppSinkEventData( GstAppSink* appSink, JsPlayer::AppSinkEvent event ) :
        appSink( appSink), event( event ) {}

    void process( JsPlayer* );

    GstAppSink* appSink;
    const JsPlayer::AppSinkEvent event;
};

void JsPlayer::AppSinkEventData::process( JsPlayer* player )
{
    switch( event ) {
     case JsPlayer::AppSinkEvent::NewPreroll:
        player->onNewPreroll( appSink );
        break;
     case JsPlayer::AppSinkEvent::NewSample:
        player->onNewSample( appSink );
        break;
     case JsPlayer::AppSinkEvent::Eos:
        player->onEos( appSink );
        break;
    }
}

///////////////////////////////////////////////////////////////////////////////
v8::Persistent<v8::Function> JsPlayer::_jsConstructor;
std::set<JsPlayer*> JsPlayer::_instances;

void JsPlayer::initJsApi( const v8::Handle<v8::Object>& exports )
{
    node::AtExit( [] ( void* ) { JsPlayer::closeAll(); } );

    gst_init( 0, 0 );

    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<FunctionTemplate> constructorTemplate = FunctionTemplate::New( isolate, jsCreate );
    constructorTemplate->SetClassName( String::NewFromUtf8( isolate, "Player", v8::String::kInternalizedString ) );

    Local<ObjectTemplate> instanceTemplate = constructorTemplate->InstanceTemplate();
    instanceTemplate->SetInternalFieldCount( 1 );

    instanceTemplate->Set( String::NewFromUtf8( isolate, "AppSinkSetup", v8::String::kInternalizedString ),
                           Integer::New( isolate, Setup ),
                           static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );
    instanceTemplate->Set( String::NewFromUtf8( isolate, "AppSinkNewPreroll", v8::String::kInternalizedString ),
                           Integer::New( isolate, NewPreroll ),
                           static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );
    instanceTemplate->Set( String::NewFromUtf8( isolate, "AppSinkNewSample", v8::String::kInternalizedString ),
                           Integer::New( isolate, NewSample ),
                           static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );
    instanceTemplate->Set( String::NewFromUtf8( isolate, "AppSinkEos", v8::String::kInternalizedString ),
                           Integer::New( isolate, Eos ),
                           static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );

    instanceTemplate->Set( String::NewFromUtf8( isolate, "GST_STATE_VOID_PENDING", v8::String::kInternalizedString ),
                           Integer::New( isolate, GST_STATE_VOID_PENDING ),
                           static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );
    instanceTemplate->Set( String::NewFromUtf8( isolate, "GST_STATE_NULL", v8::String::kInternalizedString ),
                           Integer::New( isolate, GST_STATE_NULL ),
                           static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );
    instanceTemplate->Set( String::NewFromUtf8( isolate, "GST_STATE_READY", v8::String::kInternalizedString ),
                           Integer::New( isolate, GST_STATE_READY ),
                           static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );
    instanceTemplate->Set( String::NewFromUtf8( isolate, "GST_STATE_PAUSED", v8::String::kInternalizedString ),
                           Integer::New( isolate, GST_STATE_PAUSED ),
                           static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );
    instanceTemplate->Set( String::NewFromUtf8( isolate, "GST_STATE_PLAYING", v8::String::kInternalizedString ),
                           Integer::New( isolate, GST_STATE_PLAYING ),
                           static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );

    SET_METHOD( instanceTemplate, "parseLaunch", &JsPlayer::parseLaunch );
    SET_METHOD( instanceTemplate, "addAppSinkCallback", &JsPlayer::addAppSinkCallback );
    SET_METHOD( instanceTemplate, "setState", &JsPlayer::setState );

    Local<Function> constructor = constructorTemplate->GetFunction();
    _jsConstructor.Reset( isolate, constructor );

    exports->Set( String::NewFromUtf8( isolate, "createPlayer", v8::String::kInternalizedString ), constructor );
    exports->Set( String::NewFromUtf8( isolate, "Player", v8::String::kInternalizedString ), constructor );
}

void JsPlayer::jsCreate( const v8::FunctionCallbackInfo<v8::Value>& args )
{
    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<Object> thisObject = args.Holder();
    if( args.IsConstructCall() ) {
        JsPlayer* jsPlayer = new JsPlayer( thisObject );
        args.GetReturnValue().Set( jsPlayer->handle() );
    } else {
        Local<Value> argv[] = { args[0] };
        Local<Function> constructor =
            Local<Function>::New( isolate, _jsConstructor );
        args.GetReturnValue().Set( constructor->NewInstance( sizeof( argv ) / sizeof( argv[0] ), argv ) );
    }
}

void JsPlayer::closeAll()
{
    for( JsPlayer* p : _instances ) {
        p->close();
    }

    gst_deinit();
}

JsPlayer::JsPlayer( v8::Local<v8::Object>& thisObject ) :
    _pipeline( nullptr ), _firstSample( true )
{
    Wrap( thisObject );

    _instances.insert( this );

    uv_loop_t* loop = uv_default_loop();

    uv_async_init( loop, &_async,
        [] ( uv_async_t* handle ) {
            if( handle->data )
                reinterpret_cast<JsPlayer*>( handle->data )->handleAsync();
        }
    );
    _async.data = this;

}

JsPlayer::~JsPlayer()
{
    close();

    _instances.erase( this );
}

void JsPlayer::close()
{
    _async.data = nullptr;
    uv_close( reinterpret_cast<uv_handle_t*>( &_async ), 0 );
}

void JsPlayer::handleAsync()
{
    while( !_asyncData.empty() ) {
        std::deque<std::unique_ptr<AsyncData> > tmpData;
        _asyncDataGuard.lock();
        _asyncData.swap( tmpData );
        _asyncDataGuard.unlock();

        for( const auto& i: tmpData ) {
            i->process( this );
        }
    }
}

GstFlowReturn JsPlayer::onNewPrerollProxy( GstAppSink *appSink, gpointer userData )
{
    JsPlayer* player = static_cast<JsPlayer*>( userData );

    player->_asyncDataGuard.lock();
    player->_asyncData.emplace_back( new AppSinkEventData( appSink, NewPreroll ) );
    player->_asyncDataGuard.unlock();
    uv_async_send( &player->_async );

    return GST_FLOW_OK;
}

GstFlowReturn JsPlayer::onNewSampleProxy( GstAppSink *appSink, gpointer userData )
{
    JsPlayer* player = static_cast<JsPlayer*>( userData );

    player->_appSinks[appSink].waitingSample.clear();
    player->_asyncDataGuard.lock();
    player->_asyncData.emplace_back( new AppSinkEventData( appSink, NewSample ) );
    player->_asyncDataGuard.unlock();
    uv_async_send( &player->_async );

    return GST_FLOW_OK;
}

void JsPlayer::onEosProxy( GstAppSink* appSink, gpointer userData )
{
    JsPlayer* player = static_cast<JsPlayer*>( userData );

    player->_asyncDataGuard.lock();
    player->_asyncData.emplace_back( new AppSinkEventData( appSink, Eos ) );
    player->_asyncDataGuard.unlock();
    uv_async_send( &player->_async );
}

void JsPlayer::callCallback( GstAppSink* appSink, JsPlayer::AppSinkEvent event,
                             std::initializer_list<v8::Local<v8::Value> > list )
{
    auto it = _appSinks.find( appSink );
    if( _appSinks.end() == it )
        return;

    if( it->second.callback.IsEmpty() )
        return;

    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    Local<Function> callbackFunc =
        Local<Function>::New( isolate, _appSinks[appSink].callback );

    std::vector<v8::Local<v8::Value> > argList;

    argList.push_back( Integer::New( isolate, event ) );
    argList.insert( argList.end(), list );
    callbackFunc->Call( handle(), static_cast<int>( argList.size() ), argList.data() );
}

void JsPlayer::onSetup( GstAppSink* appSink, const GstVideoInfo& videoInfo )
{
    if( !appSink )
        return;

    using namespace v8;

    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    _firstSample = false;

    callCallback( appSink, Setup,
                  { String::NewFromUtf8( isolate, videoInfo.finfo->name, v8::String::kNormalString ),
                    Integer::New( isolate, videoInfo.width ),
                    Integer::New( isolate, videoInfo.height ),
                    Integer::New( isolate, videoInfo.finfo->format ) } );
}

void JsPlayer::onNewPreroll( GstAppSink* appSink )
{
    GstSample* sample = gst_app_sink_pull_preroll( appSink );

    onSample( appSink, sample, true );

    gst_sample_unref( sample );
}

void JsPlayer::onNewSample( GstAppSink* appSink )
{
    if( _appSinks[appSink].waitingSample.test_and_set() )
        return;

    GstSample* sample = gst_app_sink_pull_sample( appSink );

    _appSinks[appSink].waitingSample.test_and_set();

    onSample( appSink, sample, false );

    gst_sample_unref( sample );
}

void JsPlayer::onSample( GstAppSink* appSink, GstSample* sample, bool preroll )
{
    if( !sample )
        return;

    GstVideoInfo videoInfo;

    GstCaps* caps = gst_sample_get_caps( sample );
    if( !caps || !gst_video_info_from_caps( &videoInfo, caps ) )
        return;

    if( _firstSample )
        onSetup( appSink, videoInfo );

    GstBuffer* buffer = gst_sample_get_buffer( sample );

    if( !buffer )
        return;

    GstMapInfo mapInfo;
    if( gst_buffer_map( buffer, &mapInfo, GST_MAP_READ ) ) {
        using namespace v8;

        Isolate* isolate = Isolate::GetCurrent();
        HandleScope scope( isolate );

#if USE_MAYBE_LOCAL
        v8::MaybeLocal<v8::Object> maybeFrame =
#else
        v8::Local<v8::Object> frame =
#endif
            node::Buffer::New( isolate, reinterpret_cast<char*>( mapInfo.data ), mapInfo.size,
                               [] ( char* data, void* hint ) {}, nullptr );

#if USE_MAYBE_LOCAL
        Local<v8::Object> frame;
        if( maybeFrame.ToLocal( &frame ) )
#endif
        {
            frame->ForceSet( String::NewFromUtf8( isolate, "pixelFormatName", v8::String::kInternalizedString ),
                             String::NewFromUtf8( isolate, videoInfo.finfo->name, v8::String::kNormalString ),
                             static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );
            frame->ForceSet( String::NewFromUtf8( isolate, "pixelFormat", v8::String::kInternalizedString ),
                             Integer::New( isolate, videoInfo.finfo->format ),
                             static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );

            frame->ForceSet( String::NewFromUtf8( isolate, "width", v8::String::kInternalizedString ),
                             Integer::New( isolate, videoInfo.width ),
                             static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );
            frame->ForceSet( String::NewFromUtf8( isolate, "height", v8::String::kInternalizedString ),
                             Integer::New( isolate, videoInfo.height ),
                             static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );

            if( !GST_BUFFER_FLAG_IS_SET( buffer, GST_BUFFER_FLAG_DELTA_UNIT ) ) {
                frame->ForceSet( String::NewFromUtf8( isolate, "key", v8::String::kInternalizedString ),
                                 Boolean::New( isolate, true ),
                                 static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );
            }

            if( GST_BUFFER_FLAG_IS_SET( buffer, GST_BUFFER_FLAG_HEADER ) ) {
                frame->ForceSet( String::NewFromUtf8( isolate, "header", v8::String::kInternalizedString ),
                                 Boolean::New( isolate, true ),
                                 static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );
            }

            if( GST_BUFFER_DTS_IS_VALID( buffer ) ) {
                frame->ForceSet( String::NewFromUtf8( isolate, "dts", v8::String::kInternalizedString ),
                                 Number::New( isolate, static_cast<double>( GST_BUFFER_DTS( buffer ) ) ),
                                 static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );
            }

            Local<Array> planes = Array::New( isolate, videoInfo.finfo->n_planes );
            for( guint p = 0; p < videoInfo.finfo->n_planes; ++p ) {
                planes->Set( p, Integer::New( isolate, videoInfo.offset[p] ) );
            }

            frame->ForceSet( String::NewFromUtf8( isolate, "planes", v8::String::kInternalizedString ),
                             planes,
                             static_cast<v8::PropertyAttribute>( ReadOnly | DontDelete ) );

            callCallback( appSink, ( preroll ? NewPreroll : NewSample ), { frame } );
        }

        gst_buffer_unmap( buffer, &mapInfo );
    }
}

void JsPlayer::onEos( GstAppSink* appSink )
{
    callCallback( appSink, Eos );
}

bool JsPlayer::parseLaunch( const std::string& pipelineDescription )
{
    if( _pipeline ) {
        _appSinks.clear();
        gst_object_unref( _pipeline );
        _pipeline = nullptr;
        _firstSample = true;
    }

    GError* error = nullptr;
    _pipeline = gst_parse_launch( pipelineDescription.c_str(), &error );

    return ( nullptr != _pipeline );
}

bool JsPlayer::addAppSinkCallback( const std::string& appSinkName, v8::Local<v8::Value> value )
{
    if( !_pipeline || appSinkName.empty() )
        return false;

    GstElement* sink = gst_bin_get_by_name( GST_BIN( _pipeline ), appSinkName.c_str() );
    if( !sink )
        return false;

    GstAppSink* appSink = GST_APP_SINK_CAST( sink );
    if( !appSink )
        return appSink;

    using namespace v8;

    Local<Function> callbackFunc = Local<Function>::Cast( value );
    if( callbackFunc.IsEmpty() )
        return false;

    auto it = _appSinks.find( appSink );
    if( _appSinks.end() == it ) {
        gst_app_sink_set_drop( appSink, true );
        gst_app_sink_set_max_buffers( appSink, 1 );
        GstAppSinkCallbacks callbacks = { onEosProxy, onNewPrerollProxy, onNewSampleProxy };
        gst_app_sink_set_callbacks( appSink, &callbacks, this, nullptr );
        it = _appSinks.emplace( appSink, AppSinkData() ).first;
    }

    AppSinkData& sinkData = it->second;
    sinkData.callback.Reset( Isolate::GetCurrent(), callbackFunc );
    sinkData.waitingSample.test_and_set();
}

void JsPlayer::setState( unsigned state )
{
    if( _pipeline )
        gst_element_set_state( _pipeline, static_cast<GstState>( state ) );
}
