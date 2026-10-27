#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "weapp/desktop.h"
#include "weapp/desktop.hpp"
using namespace weapp::desktop;
struct weapp_desktop_session {
  std::shared_ptr<Session> session;
};
struct weapp_desktop_view {
  std::unique_ptr<View> view;
  std::thread::id owner;
};
struct weapp_desktop_frame {
  Frame frame;
};
// All Value nodes in C use actual allocated wrappers. No type-punning a C++
// Value as an opaque handle; child caches hold stable borrowed wrapper nodes.
struct weapp_desktop_value {
  Value value;
  mutable std::map<std::string, std::unique_ptr<weapp_desktop_value>>
      object_cache;
  mutable std::map<size_t, std::unique_ptr<weapp_desktop_value>> array_cache;
  explicit weapp_desktop_value(Value v = {}) : value(std::move(v)) {}
};
static const char* str(const char* s) { return s ? s : ""; }
static void copy(char* out, size_t n, const std::string& s) {
  if (n) {
    std::strncpy(out, s.c_str(), n - 1);
    out[n - 1] = 0;
  }
}
static int result(const Status& s, weapp_desktop_error* e) {
  if (e) {
    *e = {};
    copy(e->code, sizeof(e->code), s.code);
    copy(e->message, sizeof(e->message), s.message);
  }
  return s ? 0 : -1;
}
template <class F>
static int guard(weapp_desktop_error* e, F f) {
  try {
    return result(f(), e);
  } catch (const std::exception& x) {
    return result(Status::error("ERR_INVALID_ARGUMENT", x.what()), e);
  } catch (...) {
    return result(Status::error("ERR_NATIVE", "unexpected desktop exception"),
                  e);
  }
}
static Session& session(weapp_desktop_session* s) {
  if (!s) throw std::invalid_argument("session is null");
  return *s->session;
}
static Value value(const weapp_desktop_value* p) {
  return p ? p->value : Value{};
}
static Value::Object object(const weapp_desktop_value* p) {
  if (!p) return {};
  auto* o = std::get_if<Value::Object>(&p->value.data);
  if (!o) throw std::invalid_argument("value must be an object");
  return *o;
}
static Metrics metrics(const weapp_desktop_metrics& m) {
  return {m.width,        m.height,        m.pixel_ratio,
          m.screen_width, m.screen_height, m.screen_top};
}
static Rect rect(weapp_desktop_rect r) { return {r.x, r.y, r.width, r.height}; }
static View& view(weapp_desktop_view* v) {
  if (!v) throw std::invalid_argument("view is null");
  if (v->owner != std::this_thread::get_id())
    throw std::invalid_argument("view belongs to its creating UI thread");
  return *v->view;
}
extern "C" {
weapp_desktop_value* weapp_desktop_value_null(void) {
  try {
    return new weapp_desktop_value;
  } catch (...) {
    return nullptr;
  }
}
weapp_desktop_value* weapp_desktop_value_bool(int v) {
  try {
    return new weapp_desktop_value(bool(v));
  } catch (...) {
    return nullptr;
  }
}
weapp_desktop_value* weapp_desktop_value_number(double v) {
  try {
    return std::isfinite(v) ? new weapp_desktop_value(v) : nullptr;
  } catch (...) {
    return nullptr;
  }
}
weapp_desktop_value* weapp_desktop_value_string(const char* v) {
  try {
    return new weapp_desktop_value(str(v));
  } catch (...) {
    return nullptr;
  }
}
weapp_desktop_value* weapp_desktop_value_array(void) {
  try {
    return new weapp_desktop_value(Value::Array{});
  } catch (...) {
    return nullptr;
  }
}
weapp_desktop_value* weapp_desktop_value_object(void) {
  try {
    return new weapp_desktop_value(Value::Object{});
  } catch (...) {
    return nullptr;
  }
}
weapp_desktop_value* weapp_desktop_value_clone(const weapp_desktop_value* v) {
  try {
    return new weapp_desktop_value(value(v));
  } catch (...) {
    return nullptr;
  }
}
void weapp_desktop_value_destroy(weapp_desktop_value* v) { delete v; }
weapp_desktop_value_kind weapp_desktop_value_type(
    const weapp_desktop_value* v) {
  return v ? static_cast<weapp_desktop_value_kind>(v->value.data.index())
           : WEAPP_DESKTOP_NULL;
}
int weapp_desktop_value_get_bool(const weapp_desktop_value* v) {
  auto* p = v ? std::get_if<bool>(&v->value.data) : nullptr;
  return p && *p;
}
double weapp_desktop_value_get_number(const weapp_desktop_value* v) {
  auto* p = v ? std::get_if<double>(&v->value.data) : nullptr;
  return p ? *p : 0;
}
const char* weapp_desktop_value_get_string(const weapp_desktop_value* v) {
  auto* p = v ? std::get_if<std::string>(&v->value.data) : nullptr;
  return p ? p->c_str() : nullptr;
}
size_t weapp_desktop_value_size(const weapp_desktop_value* v) {
  if (!v) return 0;
  if (auto* a = std::get_if<Value::Array>(&v->value.data)) return a->size();
  if (auto* o = std::get_if<Value::Object>(&v->value.data)) return o->size();
  return 0;
}
const weapp_desktop_value* weapp_desktop_value_at(const weapp_desktop_value* v,
                                                  size_t i) {
  try {
    if (!v) return nullptr;
    auto* a = std::get_if<Value::Array>(&v->value.data);
    if (!a || i >= a->size()) return nullptr;
    auto& p = v->array_cache[i];
    if (!p) p = std::make_unique<weapp_desktop_value>((*a)[i]);
    return p.get();
  } catch (...) {
    return nullptr;
  }
}
const weapp_desktop_value* weapp_desktop_value_get(const weapp_desktop_value* v,
                                                   const char* key) {
  try {
    if (!v || !key) return nullptr;
    auto* o = std::get_if<Value::Object>(&v->value.data);
    if (!o) return nullptr;
    auto i = o->find(key);
    if (i == o->end()) return nullptr;
    auto& p = v->object_cache[key];
    if (!p) p = std::make_unique<weapp_desktop_value>(i->second);
    return p.get();
  } catch (...) {
    return nullptr;
  }
}
const char* weapp_desktop_value_key_at(const weapp_desktop_value* v, size_t n) {
  if (!v) return nullptr;
  auto* o = std::get_if<Value::Object>(&v->value.data);
  if (!o || n >= o->size()) return nullptr;
  auto i = o->begin();
  std::advance(i, n);
  return i->first.c_str();
}
int weapp_desktop_value_append(weapp_desktop_value* v,
                               const weapp_desktop_value* item) {
  try {
    if (!v) return -1;
    auto* a = std::get_if<Value::Array>(&v->value.data);
    if (!a) return -1;
    a->push_back(value(item));
    v->array_cache.clear();
    return 0;
  } catch (...) {
    return -1;
  }
}
int weapp_desktop_value_set(weapp_desktop_value* v, const char* key,
                            const weapp_desktop_value* item) {
  try {
    if (!v || !key) return -1;
    auto* o = std::get_if<Value::Object>(&v->value.data);
    if (!o) return -1;
    (*o)[key] = value(item);
    v->object_cache.clear();
    return 0;
  } catch (...) {
    return -1;
  }
}
int weapp_desktop_session_create(const weapp_desktop_config* c,
                                 weapp_desktop_session** out,
                                 weapp_desktop_error* e) {
  return guard(e, [&] {
    if (!out || *out)
      throw std::invalid_argument("output must be an empty session slot");
    Config cfg;
    if (c) {
      cfg.metrics = metrics(c->metrics);
      cfg.storage_key = str(c->storage_key);
      cfg.storage_root = str(c->storage_root);
      cfg.session_id = str(c->session_id);
    }
    Status s;
    auto native = Session::create(cfg, s);
    if (!s) return s;
    auto wrapper = std::make_unique<weapp_desktop_session>();
    wrapper->session = std::move(native);
    *out = wrapper.release();
    return Status{};
  });
}
int weapp_desktop_session_on_event(weapp_desktop_session* s,
                                   weapp_desktop_event_callback callback,
                                   void* context, weapp_desktop_error* e) {
  return guard(e, [&] {
    session(s).on_event(
        callback ? std::function<void(const Event&)>([=](const Event& event) {
          weapp_desktop_value fields(event.fields);
          callback(context, static_cast<weapp_desktop_event_type>(event.type),
                   &fields);
        })
                 : nullptr);
    return Status{};
  });
}
int weapp_desktop_session_load(weapp_desktop_session* s, const char* path,
                               const char* route,
                               const weapp_desktop_value* opts,
                               weapp_desktop_error* e) {
  return guard(e, [&] {
    return session(s).load(str(path), {str(route), object(opts)});
  });
}
int weapp_desktop_session_navigate(weapp_desktop_session* s, const char* route,
                                   const weapp_desktop_value* query,
                                   int replace, weapp_desktop_error* e) {
  return guard(e, [&] {
    return session(s).navigate(str(route), object(query), bool(replace));
  });
}
int weapp_desktop_session_back(weapp_desktop_session* s,
                               weapp_desktop_error* e) {
  return guard(e, [&] { return session(s).back(); });
}
int weapp_desktop_session_set_foreground(weapp_desktop_session* s, int v,
                                         weapp_desktop_error* e) {
  return guard(e, [&] { return session(s).set_foreground(bool(v)); });
}
int weapp_desktop_session_set_metrics(weapp_desktop_session* s,
                                      const weapp_desktop_metrics* m,
                                      weapp_desktop_error* e) {
  return guard(e, [&] {
    if (!m) throw std::invalid_argument("metrics are null");
    return session(s).set_metrics(metrics(*m));
  });
}
int weapp_desktop_session_send_message(weapp_desktop_session* s,
                                       const weapp_desktop_value* v,
                                       weapp_desktop_error* e) {
  return guard(e, [&] { return session(s).send_message(value(v)); });
}
int weapp_desktop_session_register_host_method(weapp_desktop_session* s,
                                               const char* name,
                                               weapp_desktop_error* e) {
  return guard(e, [&] { return session(s).register_host_method(str(name)); });
}
int weapp_desktop_session_complete_host_call(weapp_desktop_session* s,
                                             const char* id,
                                             const weapp_desktop_value* v,
                                             weapp_desktop_error* e) {
  return guard(
      e, [&] { return session(s).complete_host_call(str(id), value(v)); });
}
int weapp_desktop_session_fail_host_call(weapp_desktop_session* s,
                                         const char* id, const char* m,
                                         weapp_desktop_error* e) {
  return guard(e, [&] { return session(s).fail_host_call(str(id), str(m)); });
}
int weapp_desktop_session_pointer(weapp_desktop_session* s,
                                  weapp_desktop_pointer_action a, uint32_t id,
                                  double x, double y, weapp_desktop_error* e) {
  return guard(e, [&] {
    if (a < WEAPP_DESKTOP_POINTER_DOWN || a > WEAPP_DESKTOP_POINTER_CANCEL)
      throw std::invalid_argument("invalid pointer action");
    return session(s).pointer({static_cast<PointerAction>(a), id, x, y});
  });
}
int weapp_desktop_session_wheel(weapp_desktop_session* s, double x, double y,
                                double dx, double dy, weapp_desktop_error* e) {
  return guard(e, [&] { return session(s).wheel({x, y, dx, dy}); });
}
int weapp_desktop_session_key(weapp_desktop_session* s, int down,
                              const char* key, const char* code, int repeat,
                              int shift, int control, int alt, int meta,
                              weapp_desktop_error* e) {
  return guard(e, [&] {
    return session(s).key({bool(down), bool(repeat), str(key), str(code),
                           bool(shift), bool(control), bool(alt), bool(meta)});
  });
}
int weapp_desktop_session_flush_storage(weapp_desktop_session* s,
                                        weapp_desktop_error* e) {
  return guard(e, [&] { return session(s).flush_storage(); });
}
int weapp_desktop_session_close(weapp_desktop_session* s,
                                weapp_desktop_error* e) {
  return guard(e, [&] { return session(s).close(); });
}
int weapp_desktop_session_pump(weapp_desktop_session* s,
                               weapp_desktop_error* e) {
  return guard(e, [&] { return session(s).pump(); });
}
int weapp_desktop_session_get_state(weapp_desktop_session* s,
                                    weapp_desktop_state* out,
                                    weapp_desktop_error* e) {
  return guard(e, [&] {
    if (!out) throw std::invalid_argument("state output is null");
    auto v = session(s).state();
    *out = {};
    out->status = static_cast<weapp_desktop_state_kind>(v.status);
    out->generation = v.generation;
    out->revision = v.revision;
    out->foreground = v.foreground;
    copy(out->route, sizeof(out->route), v.route);
    return Status{};
  });
}
int weapp_desktop_session_capabilities(weapp_desktop_session* s,
                                       weapp_desktop_capabilities* out,
                                       weapp_desktop_error* e) {
  return guard(e, [&] {
    if (!out) throw std::invalid_argument("capabilities output is null");
    Capabilities value;
    auto status = session(s).capabilities(value);
    if (!status) return status;
    *out = {};
    out->quickjs = value.quickjs;
    out->skia_cpu = value.skia_cpu;
    out->metal = value.metal;
    out->gpu_presented = value.gpu_presented;
    copy(out->renderer, sizeof(out->renderer), value.renderer);
    return Status{};
  });
}
int weapp_desktop_session_destroy(weapp_desktop_session** s,
                                  weapp_desktop_error* e) {
  return guard(e, [&] {
    if (!s) throw std::invalid_argument("session slot is null");
    if (!*s) return Status{};
    if (session(*s).state().status != SessionState::Closed)
      return Status::error("ERR_CLOSE_PENDING",
                           "close and await Closed before destroy");
    session(*s).on_event(nullptr);
    delete *s;
    *s = nullptr;
    return Status{};
  });
}
int weapp_desktop_view_create(weapp_desktop_session* s, void* parent,
                              weapp_desktop_rect r, weapp_desktop_view** out,
                              weapp_desktop_error* e) {
  return guard(e, [&] {
    if (!s || !out || *out)
      throw std::invalid_argument("session and empty view slot are required");
    Status status;
    auto v = View::create(s->session, parent, rect(r), status);
    if (!status) return status;
    auto wrapper = std::make_unique<weapp_desktop_view>();
    wrapper->view = std::move(v);
    wrapper->owner = std::this_thread::get_id();
    *out = wrapper.release();
    return Status{};
  });
}
void* weapp_desktop_view_native_handle(weapp_desktop_view* v) {
  try {
    return view(v).native_handle();
  } catch (...) {
    return nullptr;
  }
}
int weapp_desktop_view_set_bounds(weapp_desktop_view* v, weapp_desktop_rect r,
                                  weapp_desktop_error* e) {
  return guard(e, [&] { return view(v).set_bounds(rect(r)); });
}
int weapp_desktop_view_set_visible(weapp_desktop_view* v, int visible,
                                   weapp_desktop_error* e) {
  return guard(e, [&] { return view(v).set_visible(bool(visible)); });
}
int weapp_desktop_view_destroy(weapp_desktop_view** v, weapp_desktop_error* e) {
  return guard(e, [&] {
    if (!v) throw std::invalid_argument("view slot is null");
    if (*v) {
      view(*v);
      delete *v;
      *v = nullptr;
    }
    return Status{};
  });
}
int weapp_desktop_session_snapshot(weapp_desktop_session* s,
                                   weapp_desktop_frame** out,
                                   weapp_desktop_error* e) {
  return guard(e, [&] {
    if (!out || *out) throw std::invalid_argument("frame output must be empty");
    auto f = std::make_unique<weapp_desktop_frame>();
    auto status = session(s).snapshot(f->frame);
    if (status) *out = f.release();
    return status;
  });
}
int weapp_desktop_frame_get_info(const weapp_desktop_frame* f,
                                 weapp_desktop_frame_info* out) {
  if (!f || !out) return -1;
  *out = {f->frame.pixels(), f->frame.byte_size(), f->frame.width(),
          f->frame.height(), f->frame.stride(),    f->frame.revision()};
  return 0;
}
void weapp_desktop_frame_destroy(weapp_desktop_frame* f) { delete f; }
}  // extern C
