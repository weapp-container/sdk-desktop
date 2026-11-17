#include <stdio.h>
#include <string.h>
#include <weapp/desktop.h>
// This translation unit is compiled as C, against installed headers only.
int c_consumer_smoke(void) {
  weapp_desktop_value* object = weapp_desktop_value_object();
  weapp_desktop_value* text =
      weapp_desktop_value_string("业务数据 / \xF0\x9F\x8C\x8D");
  if (!object || !text || weapp_desktop_value_set(object, "hello", text))
    return 1;
  const char* value =
      weapp_desktop_value_get_string(weapp_desktop_value_get(object, "hello"));
  int failed = !value || strcmp(value, "业务数据 / \xF0\x9F\x8C\x8D");
  weapp_desktop_value_destroy(text);
  weapp_desktop_value_destroy(object);
  weapp_desktop_session* session = NULL;
  weapp_desktop_error error = {0};
  if (weapp_desktop_session_create(NULL, &session, &error)) return 2;
  if (weapp_desktop_session_close(session, &error)) return 3;
  if (weapp_desktop_session_destroy(&session, &error) || session) return 4;
  return failed;
}
