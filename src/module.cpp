#include <napi.h>

#include "JsPlayer.h"


Napi::Object Init(Napi::Env env, Napi::Object exports)
{
	return JsPlayer::InitJsApi(env, exports);
}

NODE_API_MODULE(wcjs_gs, Init)
