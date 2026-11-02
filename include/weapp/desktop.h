#ifndef WEAPP_DESKTOP_H
#define WEAPP_DESKTOP_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define WEAPP_DESKTOP_VERSION "0.1.0-preview.6.0"
typedef struct weapp_desktop_session weapp_desktop_session;
typedef struct weapp_desktop_view weapp_desktop_view;
typedef struct weapp_desktop_value weapp_desktop_value;
typedef struct weapp_desktop_frame weapp_desktop_frame;
typedef struct {
  char code[64];
  char message[1024];
} weapp_desktop_error;
typedef struct {
  uint32_t width, height;
  double pixel_ratio;
  uint32_t screen_width, screen_height;
  double screen_top;
} weapp_desktop_metrics;
typedef struct {
  weapp_desktop_metrics metrics;
  const char *storage_key, *storage_root, *session_id;
} weapp_desktop_config;
typedef struct {
  double x, y, width, height;
} weapp_desktop_rect;
typedef enum {
  WEAPP_DESKTOP_EMPTY,
  WEAPP_DESKTOP_READY,
  WEAPP_DESKTOP_CLOSING,
  WEAPP_DESKTOP_CLOSED
} weapp_desktop_state_kind;
typedef struct {
  weapp_desktop_state_kind status;
  uint64_t generation, revision;
  int foreground;
  char route[1024];
} weapp_desktop_state;
typedef enum {
  WEAPP_DESKTOP_EVENT_READY,
  WEAPP_DESKTOP_EVENT_NAVIGATION,
  WEAPP_DESKTOP_EVENT_FRAME,
  WEAPP_DESKTOP_EVENT_MESSAGE,
  WEAPP_DESKTOP_EVENT_HOST_CALL,
  WEAPP_DESKTOP_EVENT_HOST_CALL_CANCELLED,
  WEAPP_DESKTOP_EVENT_ERROR,
  WEAPP_DESKTOP_EVENT_STORAGE_FLUSHED,
  WEAPP_DESKTOP_EVENT_CLOSED,
  WEAPP_DESKTOP_EVENT_UNKNOWN
} weapp_desktop_event_type;
// Event fields are a borrowed typed object valid only during the callback.
// Clone if retaining. Callbacks run on the creating UI thread, outside runtime
// calls.
typedef void (*weapp_desktop_event_callback)(void*, weapp_desktop_event_type,
                                             const weapp_desktop_value* fields);
typedef enum {
  WEAPP_DESKTOP_NULL,
  WEAPP_DESKTOP_BOOL,
  WEAPP_DESKTOP_NUMBER,
  WEAPP_DESKTOP_STRING,
  WEAPP_DESKTOP_ARRAY,
  WEAPP_DESKTOP_OBJECT
} weapp_desktop_value_kind;
weapp_desktop_value* weapp_desktop_value_null(void);
weapp_desktop_value* weapp_desktop_value_bool(int);
weapp_desktop_value* weapp_desktop_value_number(double);
weapp_desktop_value* weapp_desktop_value_string(const char* utf8);
weapp_desktop_value* weapp_desktop_value_array(void);
weapp_desktop_value* weapp_desktop_value_object(void);
weapp_desktop_value* weapp_desktop_value_clone(const weapp_desktop_value*);
void weapp_desktop_value_destroy(weapp_desktop_value*);
weapp_desktop_value_kind weapp_desktop_value_type(const weapp_desktop_value*);
int weapp_desktop_value_get_bool(const weapp_desktop_value*);
double weapp_desktop_value_get_number(const weapp_desktop_value*);
const char* weapp_desktop_value_get_string(const weapp_desktop_value*);
size_t weapp_desktop_value_size(const weapp_desktop_value*);
// Child access returns a borrowed node. Mutating the parent invalidates
// borrowed nodes.
const weapp_desktop_value* weapp_desktop_value_at(const weapp_desktop_value*,
                                                  size_t);
const weapp_desktop_value* weapp_desktop_value_get(const weapp_desktop_value*,
                                                   const char* key);
const char* weapp_desktop_value_key_at(const weapp_desktop_value*, size_t);
int weapp_desktop_value_append(weapp_desktop_value*,
                               const weapp_desktop_value*);
int weapp_desktop_value_set(weapp_desktop_value*, const char*,
                            const weapp_desktop_value*);
// Fallible operations return 0 on success. Errors are optional output values.
// Session/View calls and destruction belong to one host UI thread. No SDK owns
// the application event loop. A view holds a reference to its session.
int weapp_desktop_session_create(const weapp_desktop_config*,
                                 weapp_desktop_session**, weapp_desktop_error*);
int weapp_desktop_session_on_event(weapp_desktop_session*,
                                   weapp_desktop_event_callback, void*,
                                   weapp_desktop_error*);
int weapp_desktop_session_load(weapp_desktop_session*, const char* directory,
                               const char* route,
                               const weapp_desktop_value* options,
                               weapp_desktop_error*);
int weapp_desktop_session_navigate(weapp_desktop_session*, const char* route,
                                   const weapp_desktop_value* query,
                                   int replace, weapp_desktop_error*);
int weapp_desktop_session_back(weapp_desktop_session*, weapp_desktop_error*);
int weapp_desktop_session_set_foreground(weapp_desktop_session*, int,
                                         weapp_desktop_error*);
int weapp_desktop_session_set_metrics(weapp_desktop_session*,
                                      const weapp_desktop_metrics*,
                                      weapp_desktop_error*);
int weapp_desktop_session_send_message(weapp_desktop_session*,
                                       const weapp_desktop_value*,
                                       weapp_desktop_error*);
int weapp_desktop_session_register_host_method(weapp_desktop_session*,
                                               const char*,
                                               weapp_desktop_error*);
int weapp_desktop_session_complete_host_call(weapp_desktop_session*,
                                             const char* call_id,
                                             const weapp_desktop_value*,
                                             weapp_desktop_error*);
int weapp_desktop_session_fail_host_call(weapp_desktop_session*,
                                         const char* call_id,
                                         const char* message,
                                         weapp_desktop_error*);
typedef enum {
  WEAPP_DESKTOP_POINTER_DOWN,
  WEAPP_DESKTOP_POINTER_MOVE,
  WEAPP_DESKTOP_POINTER_UP,
  WEAPP_DESKTOP_POINTER_CANCEL
} weapp_desktop_pointer_action;
int weapp_desktop_session_pointer(weapp_desktop_session*,
                                  weapp_desktop_pointer_action, uint32_t id,
                                  double x, double y, weapp_desktop_error*);
int weapp_desktop_session_wheel(weapp_desktop_session*, double x, double y,
                                double delta_x, double delta_y,
                                weapp_desktop_error*);
int weapp_desktop_session_key(weapp_desktop_session*, int down, const char* key,
                              const char* code, int repeat, int shift,
                              int control, int alt, int meta,
                              weapp_desktop_error*);
int weapp_desktop_session_flush_storage(weapp_desktop_session*,
                                        weapp_desktop_error*);
int weapp_desktop_session_close(weapp_desktop_session*, weapp_desktop_error*);
int weapp_desktop_session_pump(weapp_desktop_session*, weapp_desktop_error*);
int weapp_desktop_session_get_state(weapp_desktop_session*,
                                    weapp_desktop_state*, weapp_desktop_error*);
typedef struct {
  int quickjs, skia_cpu, metal, gpu_presented;
  char renderer[64];
} weapp_desktop_capabilities;
int weapp_desktop_session_capabilities(weapp_desktop_session*,
                                       weapp_desktop_capabilities*,
                                       weapp_desktop_error*);
// destroy requires CLOSED and clears the caller slot. It never silently drops a
// pending durable close; keep pumping the host loop until the Closed event.
int weapp_desktop_session_destroy(weapp_desktop_session**,
                                  weapp_desktop_error*);
// NSView*, HWND or GtkContainer* parent; result handle is NSView*, HWND or
// GtkWidget*. The host must not keep the returned handle after view
// destruction.
int weapp_desktop_view_create(weapp_desktop_session*, void* parent,
                              weapp_desktop_rect, weapp_desktop_view**,
                              weapp_desktop_error*);
void* weapp_desktop_view_native_handle(weapp_desktop_view*);
int weapp_desktop_view_set_bounds(weapp_desktop_view*, weapp_desktop_rect,
                                  weapp_desktop_error*);
int weapp_desktop_view_set_visible(weapp_desktop_view*, int,
                                   weapp_desktop_error*);
int weapp_desktop_view_destroy(weapp_desktop_view**, weapp_desktop_error*);
int weapp_desktop_session_snapshot(weapp_desktop_session*,
                                   weapp_desktop_frame**, weapp_desktop_error*);
typedef struct {
  const uint8_t* pixels;
  size_t len;
  uint32_t width, height, stride;
  uint64_t revision;
} weapp_desktop_frame_info;
// Frames contain immutable premultiplied RGBA8 and may outlive the session.
int weapp_desktop_frame_get_info(const weapp_desktop_frame*,
                                 weapp_desktop_frame_info*);
void weapp_desktop_frame_destroy(weapp_desktop_frame*);
#ifdef __cplusplus
}
#endif
#endif
