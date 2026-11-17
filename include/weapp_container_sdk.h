#ifndef WEAPP_CONTAINER_SDK_H
#define WEAPP_CONTAINER_SDK_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(WEAPP_CONTAINER_SDK_SHARED)
#define WEAPP_CONTAINER_SDK_API __declspec(dllimport)
#else
#define WEAPP_CONTAINER_SDK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WEAPP_CONTAINER_SDK_ABI_VERSION 1u
#define WEAPP_CONTAINER_SDK_OK 0
#define WEAPP_CONTAINER_SDK_ERROR (-1)
#define WEAPP_CONTAINER_SDK_INVALID_ARGUMENT (-2)

typedef struct weapp_container_sdk_session weapp_container_sdk_session;
typedef struct weapp_container_sdk_frame weapp_container_sdk_frame;

typedef struct weapp_container_sdk_buffer {
  uint8_t *data;
  size_t len;
} weapp_container_sdk_buffer;

/* Pixels are immutable row-major premultiplied RGBA8. `data` is borrowed
 * until the owning frame reference is released. `revision` is the SDK's
 * presentation identity; application frame IDs are not presentation IDs. */
typedef struct weapp_container_sdk_frame_info_data {
  const uint8_t *data;
  size_t len;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint64_t revision;
} weapp_container_sdk_frame_info_data;

/* All fallible functions require a result initialized to {NULL, 0}.
 * An occupied or null result returns INVALID_ARGUMENT before doing work.
 * Otherwise result contains UTF-8 JSON (not NUL terminated):
 *   OK:    {"ok":true,"value":...}
 *   ERROR: {"ok":false,"error":{"code":"ERR_...","message":"..."}}
 * Release results with buffer_free before reuse. Null input pointers are
 * allowed only with zero length; JSON arguments must still contain valid JSON.
 * Every pointer must designate live memory for the entire call. Do not copy
 * owning handles/buffers or race access to an output slot. No function throws
 * a Rust panic across this ABI. A session panic poisons commands; close and
 * destroy remain available for cleanup.
 *
 * Session calls and destruction MUST execute on the creating thread, without
 * reentry. A wrong-thread call returns ERR_WRONG_THREAD before runtime access.
 * Frame retain/info/release and buffer_free may execute on any thread while
 * the caller holds a live reference. Frames survive session destruction. */

WEAPP_CONTAINER_SDK_API uint32_t weapp_container_sdk_abi_version(void);
/* Static borrowed NUL-terminated UTF-8 version string; never free it. */
WEAPP_CONTAINER_SDK_API const char *weapp_container_sdk_version(void);

WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_session_new(
    const uint8_t *config, size_t config_len,
    weapp_container_sdk_session **session_out,
    weapp_container_sdk_buffer *result);
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_session_command(
    weapp_container_sdk_session *session,
    const uint8_t *operation, size_t operation_len,
    const uint8_t *payload, size_t payload_len,
    weapp_container_sdk_buffer *result);

/* close is the synchronous final-cleanup primitive; platform UI wrappers should
 * use command("close", {}) and pump its completion event for asynchronous
 * storage flushing. close is idempotent and keeps the handle. Detach
 * the GPU surface first (GLES abandon after context loss). An attached surface
 * prevents close and leaves the session intact. A closed session cannot load
 * another app. destroy first closes, then clears *session and frees the handle;
 * destroy of a NULL slot value succeeds. On failure the slot is retained. */
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_session_close(
    weapp_container_sdk_session *session, weapp_container_sdk_buffer *result);
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_session_destroy(
    weapp_container_sdk_session **session, weapp_container_sdk_buffer *result);
WEAPP_CONTAINER_SDK_API void weapp_container_sdk_buffer_free(
    weapp_container_sdk_buffer *buffer);

/* Acquire/retain require *frame_out == NULL. Acquire returns ERR_NO_FRAME
 * before the first completed presentation. release clears the caller's slot
 * and is idempotent when that slot is already NULL. Retain is required before
 * passing an independently owned reference to another thread. */
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_frame_acquire(
    weapp_container_sdk_session *session,
    const weapp_container_sdk_frame **frame_out,
    weapp_container_sdk_buffer *result);
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_frame_retain(
    const weapp_container_sdk_frame *frame,
    const weapp_container_sdk_frame **frame_out,
    weapp_container_sdk_buffer *result);
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_frame_release(
    const weapp_container_sdk_frame **frame,
    weapp_container_sdk_buffer *result);
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_frame_info(
    const weapp_container_sdk_frame *frame,
    weapp_container_sdk_frame_info_data *info,
    weapp_container_sdk_buffer *result);

/* GPU entrypoints are always exported. Unsupported builds return
 * ERR_UNSUPPORTED, never silently claim a GPU path. Only one surface may be
 * attached to a session. Attach/present/detach belong to the session thread.
 * GLES: the host makes its context current before attach/present/detach and
 * owns EGL surface/swap. abandon is the recovery path for a lost context.
 * Metal: device and queue must be live retained same-device objects; drawable
 * or offscreen texture must be live, non-framebuffer-only 2D BGRA8Unorm with
 * render-target usage, sample count one, and matching physical dimensions. The host owns
 * presentation, retains resources through GPU completion, and synchronizes
 * consumers: successful present submits work but does not imply completion. */
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_gles_attach(
    weapp_container_sdk_session *session, weapp_container_sdk_buffer *result);
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_gles_present(
    weapp_container_sdk_session *session, uint32_t framebuffer,
    uint32_t width, uint32_t height, weapp_container_sdk_buffer *result);
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_gles_detach(
    weapp_container_sdk_session *session, weapp_container_sdk_buffer *result);
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_gles_abandon(
    weapp_container_sdk_session *session, weapp_container_sdk_buffer *result);
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_metal_attach(
    weapp_container_sdk_session *session, const void *device, const void *queue,
    weapp_container_sdk_buffer *result);
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_metal_present(
    weapp_container_sdk_session *session, const void *texture,
    uint32_t width, uint32_t height, weapp_container_sdk_buffer *result);
WEAPP_CONTAINER_SDK_API int32_t weapp_container_sdk_metal_detach(
    weapp_container_sdk_session *session, weapp_container_sdk_buffer *result);

#ifdef __cplusplus
}
#endif
#endif
