#include <node.h>
#include <v8.h>

#include "JsPlayer.h"

void init( v8::Handle<v8::Object> exports, v8::Handle<v8::Object> module )
{
    JsPlayer::initJsApi( exports );
}

NODE_MODULE( wcjs_gs, init )
