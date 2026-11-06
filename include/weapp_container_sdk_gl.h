#ifndef WEAPP_CONTAINER_SDK_GL_H
#define WEAPP_CONTAINER_SDK_GL_H
#include "weapp_container_sdk.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Explicit GL symbol loader. Callback is called only during attach; context
 * and returned GL functions must remain live on the owner thread until detach.
 * Null callbacks are rejected; unavailable GL builds return ERR_UNSUPPORTED. */
typedef const void *(*weapp_container_sdk_gl_proc_resolver)(void *user_data,
                                                          const char *name);
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_gl_attach_with_resolver(
    weapp_container_sdk_session *session,
    weapp_container_sdk_gl_proc_resolver resolver, void *user_data,
    weapp_container_sdk_buffer *result);
#ifdef __cplusplus
}
#endif
#endif
