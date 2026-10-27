#include <gtk/gtk.h>

#include <cmath>
#include <unordered_map>

#include "internal.hpp"

namespace weapp::desktop::detail {
const char* platform_name() { return "linux"; }
class GtkLoop final : public Loop {
 public:
  explicit GtkLoop(std::function<void()> tick) {
    source_ = g_timeout_source_new(16);
    g_source_set_callback(
        source_,
        [](gpointer data) -> gboolean {
          auto callback = *static_cast<std::function<void()>*>(data);
          callback();
          return G_SOURCE_CONTINUE;
        },
        new std::function<void()>(std::move(tick)),
        [](gpointer data) {
          delete static_cast<std::function<void()>*>(data);
        });
    g_source_attach(source_, g_main_context_get_thread_default());
  }
  ~GtkLoop() override {
    g_source_destroy(source_);
    g_source_unref(source_);
  }

 private:
  GSource* source_;
};
std::unique_ptr<Loop> create_loop(std::function<void()> tick, Status& status) {
  if (!gdk_display_get_default()) {
    status = Status::error(
        "ERR_EVENT_LOOP",
        "initialize GTK3 on the host UI thread before creating a session");
    return {};
  }
  status = {};
  return std::make_unique<GtkLoop>(std::move(tick));
}
class GtkPresenter final : public Presenter {
 public:
  explicit GtkPresenter(Core& c) : Presenter(c) {}
  ~GtkPresenter() override {
    detach();
    if (widget_) {
      g_signal_handlers_disconnect_by_data(widget_, this);
      gtk_widget_destroy(widget_);
    }
    if (retained_) g_object_unref(retained_);
  }
  Status initialize(void* parent, const Rect& bounds) override {
    if (parent && !GTK_IS_CONTAINER(parent))
      return Status::error("ERR_INVALID_ARGUMENT",
                           "GTK parent must be a GtkContainer");
    widget_ = gtk_drawing_area_new();
    g_object_ref_sink(widget_);
    retained_ = widget_;
    gtk_widget_set_can_focus(widget_, TRUE);
    gtk_widget_add_events(widget_,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                              GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK |
                              GDK_SMOOTH_SCROLL_MASK | GDK_TOUCH_MASK |
                              GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK |
                              GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(widget_, "draw", G_CALLBACK(draw_cb), this);
    g_signal_connect(widget_, "size-allocate", G_CALLBACK(size_cb), this);
    g_signal_connect(widget_, "map", G_CALLBACK(visibility_cb), this);
    g_signal_connect(widget_, "unmap", G_CALLBACK(visibility_cb), this);
    g_signal_connect(widget_, "notify::scale-factor", G_CALLBACK(scale_cb),
                     this);
    g_signal_connect(widget_, "button-press-event", G_CALLBACK(button_cb),
                     this);
    g_signal_connect(widget_, "button-release-event", G_CALLBACK(button_cb),
                     this);
    g_signal_connect(widget_, "motion-notify-event", G_CALLBACK(motion_cb),
                     this);
    g_signal_connect(widget_, "scroll-event", G_CALLBACK(scroll_cb), this);
    g_signal_connect(widget_, "touch-event", G_CALLBACK(touch_cb), this);
    g_signal_connect(widget_, "key-press-event", G_CALLBACK(key_cb), this);
    g_signal_connect(widget_, "key-release-event", G_CALLBACK(key_cb), this);
    g_signal_connect(widget_, "focus-out-event", G_CALLBACK(focus_cb), this);
    g_signal_connect(widget_, "destroy", G_CALLBACK(destroy_cb), this);
    auto s = set_bounds(bounds);
    if (!s) return s;
    if (parent) {
      if (GTK_IS_FIXED(parent))
        gtk_fixed_put(GTK_FIXED(parent), widget_, gint(bounds.x),
                      gint(bounds.y));
      else
        gtk_container_add(GTK_CONTAINER(parent), widget_);
    }
    gtk_widget_show(widget_);
    update();
    return {};
  }
  void* native_handle() const override { return widget_; }
  Status set_bounds(const Rect& r) override {
    if (!widget_)
      return Status::error("ERR_VIEW_DESTROYED",
                           "native GTK widget was destroyed by its host");
    if (!std::isfinite(r.width) || !std::isfinite(r.height) ||
        r.width > 32767 || r.height > 32767)
      return Status::error("ERR_INVALID_ARGUMENT",
                           "invalid GTK view dimensions");
    positive_ = r.width >= 1 && r.height >= 1;
    gtk_widget_set_size_request(widget_, std::max(1, int(std::lround(r.width))),
                                std::max(1, int(std::lround(r.height))));
    auto* parent = gtk_widget_get_parent(widget_);
    if (parent && GTK_IS_FIXED(parent))
      gtk_fixed_move(GTK_FIXED(parent), widget_, int(r.x), int(r.y));
    initial_width_ = std::max(1, int(std::lround(r.width)));
    initial_height_ = std::max(1, int(std::lround(r.height)));
    update();
    return {};
  }
  Status set_visible(bool show) override {
    if (!widget_)
      return Status::error("ERR_VIEW_DESTROYED",
                           "native GTK widget was destroyed by its host");
    if (show)
      gtk_widget_show(widget_);
    else
      gtk_widget_hide(widget_);
    update();
    return {};
  }
  Status attach() override {
    attached_ = true;
    return {};
  }
  Status detach() override {
    attached_ = false;
    has_presented_ = false;
    raster_.clear();
    return {};
  }
  Status present() override {
    update();
    if (!widget_ || !attached_ || !core_.surface_visible) return {};
    bool changed = false;
    auto s = raster_.update(core_, changed);
    if (changed) gtk_widget_queue_draw(widget_);
    return s;
  }
  void refresh() override { update(); }
  void update() {
    if (!widget_) return;
    visibility(positive_ && gtk_widget_get_mapped(widget_) &&
               gtk_widget_is_visible(widget_));
    if (!positive_ || !has_allocation_ ||
        core_.state.status == SessionState::Closed ||
        core_.state.status == SessionState::Closing)
      return;
    auto* display = gtk_widget_get_display(widget_);
    auto* window = gtk_widget_get_window(widget_);
    GdkMonitor* monitor =
        window ? gdk_display_get_monitor_at_window(display, window)
               : gdk_display_get_primary_monitor(display);
    if (!monitor && gdk_display_get_n_monitors(display) > 0)
      monitor = gdk_display_get_monitor(display, 0);
    if (!monitor) return;
    GdkRectangle screen{};
    gdk_monitor_get_geometry(monitor, &screen);
    int width = gtk_widget_get_allocated_width(widget_),
        height = gtk_widget_get_allocated_height(widget_);
    if (!gtk_widget_get_realized(widget_)) {
      width = initial_width_;
      height = initial_height_;
    }
    if (width < 1 || height < 1) return;
    Metrics m{uint32_t(width),
              uint32_t(height),
              double(gtk_widget_get_scale_factor(widget_)),
              uint32_t(screen.width),
              uint32_t(screen.height),
              0};
    // Wayland deliberately supplies no global window origin; never perform an
    // X11-only query.
    core_.report(metrics(m));
  }

 private:
  static GtkPresenter& self(gpointer p) {
    return *static_cast<GtkPresenter*>(p);
  }
  static gboolean draw_cb(GtkWidget*, cairo_t* cr, gpointer p) {
    auto& s = self(p);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    if (s.raster_.frame) {
      auto& r = s.raster_;
      auto* surface = cairo_image_surface_create_for_data(
          r.bgra.data(), CAIRO_FORMAT_ARGB32, int(r.info.width),
          int(r.info.height), int(r.info.stride));
      cairo_surface_set_device_scale(surface,
                                     s.core_.config.metrics.pixel_ratio,
                                     s.core_.config.metrics.pixel_ratio);
      cairo_set_source_surface(cr, surface, 0, 0);
      cairo_paint(cr);
      const auto image_status = cairo_surface_status(surface);
      cairo_surface_destroy(surface);
      cairo_surface_flush(cairo_get_target(cr));
      const auto status = cairo_status(cr);
      if (status == CAIRO_STATUS_SUCCESS && image_status == CAIRO_STATUS_SUCCESS &&
          s.attached_ && s.core_.surface_visible &&
          (!s.has_presented_ || s.presented_generation_ != r.generation ||
           s.presented_revision_ != r.info.revision)) {
        auto receipt = s.core_.acknowledge_presentation(r.generation, r.info.revision);
        if (receipt) {
          s.presented_generation_ = r.generation;
          s.presented_revision_ = r.info.revision;
          s.has_presented_ = true;
        } else if (receipt.code != "ERR_CANCELLED") s.core_.report(receipt);
      } else if (status != CAIRO_STATUS_SUCCESS || image_status != CAIRO_STATUS_SUCCESS) {
        s.core_.report(Status::error("ERR_RENDER", cairo_status_to_string(
            status != CAIRO_STATUS_SUCCESS ? status : image_status)));
      }
    }
    return TRUE;
  }
  static void size_cb(GtkWidget*, GtkAllocation* allocation, gpointer p) {
    self(p).has_allocation_ = allocation->width > 0 && allocation->height > 0;
    self(p).update();
  }
  static void visibility_cb(GtkWidget*, gpointer p) { self(p).update(); }
  static void scale_cb(GObject*, GParamSpec*, gpointer p) { self(p).update(); }
  static gboolean button_cb(GtkWidget* w, GdkEventButton* e, gpointer p) {
    if (e->button != 1) return FALSE;
    // Touch devices send their own GdkEventTouch stream; ignore emulated mouse
    // events.
    if (gdk_event_get_pointer_emulated(reinterpret_cast<GdkEvent*>(e)))
      return TRUE;
    if (e->type != GDK_BUTTON_PRESS && e->type != GDK_BUTTON_RELEASE)
      return TRUE;
    gtk_widget_grab_focus(w);
    auto& s = self(p);
    s.core_.report(s.core_.pointer(
        {e->type == GDK_BUTTON_PRESS ? PointerAction::Down : PointerAction::Up,
         0, e->x, e->y}));
    return TRUE;
  }
  static gboolean motion_cb(GtkWidget*, GdkEventMotion* e, gpointer p) {
    auto& s = self(p);
    if ((e->state & GDK_BUTTON1_MASK) &&
        !gdk_event_get_pointer_emulated(reinterpret_cast<GdkEvent*>(e)))
      s.core_.report(s.core_.pointer({PointerAction::Move, 0, e->x, e->y}));
    return TRUE;
  }
  static gboolean scroll_cb(GtkWidget*, GdkEventScroll* e, gpointer p) {
    double dx = 0, dy = 0;
    if (e->direction == GDK_SCROLL_SMOOTH) {
      dx = e->delta_x * 40;
      dy = e->delta_y * 40;
    } else {
      if (e->direction == GDK_SCROLL_UP) dy = -40;
      if (e->direction == GDK_SCROLL_DOWN) dy = 40;
      if (e->direction == GDK_SCROLL_LEFT) dx = -40;
      if (e->direction == GDK_SCROLL_RIGHT) dx = 40;
    }
    auto& s = self(p);
    s.core_.report(s.core_.wheel({e->x, e->y, dx, dy}));
    return TRUE;
  }
  static gboolean touch_cb(GtkWidget* w, GdkEventTouch* e, gpointer p) {
    auto& s = self(p);
    gtk_widget_grab_focus(w);
    if (e->type == GDK_TOUCH_BEGIN) s.touch_ids_[e->sequence] = ++s.next_touch_;
    auto it = s.touch_ids_.find(e->sequence);
    if (it == s.touch_ids_.end()) return TRUE;
    auto a = e->type == GDK_TOUCH_BEGIN    ? PointerAction::Down
             : e->type == GDK_TOUCH_UPDATE ? PointerAction::Move
             : e->type == GDK_TOUCH_END    ? PointerAction::Up
                                           : PointerAction::Cancel;
    s.core_.report(s.core_.pointer({a, it->second, e->x, e->y}));
    if (a == PointerAction::Up || a == PointerAction::Cancel)
      s.touch_ids_.erase(it);
    return TRUE;
  }
  static gboolean key_cb(GtkWidget*, GdkEventKey* e, gpointer p) {
    auto& s = self(p);
    const char* name = gdk_keyval_name(e->keyval);
    std::string key = name ? name : "";
    if (key == "Left" || key == "Right" || key == "Up" || key == "Down")
      key = "Arrow" + key;
    if (key == "Return") key = "Enter";
    if (key == "BackSpace") key = "Backspace";
    s.core_.report(s.core_.key(
        {e->type == GDK_KEY_PRESS, false, key,
         "Native" + std::to_string(e->hardware_keycode),
         bool(e->state & GDK_SHIFT_MASK), bool(e->state & GDK_CONTROL_MASK),
         bool(e->state & GDK_MOD1_MASK), bool(e->state & GDK_META_MASK)}));
    return TRUE;
  }
  static gboolean focus_cb(GtkWidget*, GdkEventFocus*, gpointer p) {
    self(p).core_.cancel_touches();
    return FALSE;
  }
  static void destroy_cb(GtkWidget* w, gpointer p) {
    auto& s = self(p);
    s.detach();
    s.visibility(false);
    g_signal_handlers_disconnect_by_data(w, p);
    s.widget_ = nullptr;
  }
  GtkWidget* widget_ = nullptr;
  GtkWidget* retained_ = nullptr;
  Raster raster_;
  bool attached_ = false, positive_ = true, has_allocation_ = false;
  bool has_presented_ = false;
  uint64_t presented_generation_ = 0, presented_revision_ = 0;
  int initial_width_ = 1, initial_height_ = 1;
  uint32_t next_touch_ = 0;
  std::unordered_map<GdkEventSequence*, uint32_t> touch_ids_;
};
std::unique_ptr<Presenter> create_presenter(Core& core) {
  return std::make_unique<GtkPresenter>(core);
}
}  // namespace weapp::desktop::detail
