#include <gtk/gtk.h>

#include "smoke.hpp"
int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: desktop_consumer PACK [REPORT]\n";
    return 2;
  }
  const std::string pack = argv[1], report = argc > 2 ? argv[2] : "";
  if (!gtk_init_check(&argc, &argv)) {
    std::cerr << "GTK display unavailable\n";
    return 3;
  }
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window),
                       "Weapp Container installed consumer");
  gtk_window_set_default_size(GTK_WINDOW(window), 760, 460);
  GtkWidget* fixed = gtk_fixed_new();
  gtk_container_add(GTK_CONTAINER(window), fixed);
  gtk_widget_show_all(window);
  Host host;
  host.parent = fixed;
  host.spin = [](double seconds) {
    auto until = std::chrono::steady_clock::now() +
                 std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
      while (g_main_context_iteration(nullptr, FALSE)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  host.laid_out = [](View& view) {
    auto* widget = GTK_WIDGET(view.native_handle());
    // A size request is not an allocation. Observe the real mapped widget
    // before load(), so the presenter has supplied viewport/display metrics.
    return gtk_widget_get_realized(widget) && gtk_widget_get_mapped(widget) &&
           gtk_widget_get_allocated_width(widget) > 1 &&
           gtk_widget_get_allocated_height(widget) > 1;
  };
  host.click = [](View& view, double x, double y) {
    auto* w = GTK_WIDGET(view.native_handle());
    GdkEvent* e = gdk_event_new(GDK_BUTTON_PRESS);
    e->button.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(w)));
    e->button.send_event = TRUE;
    e->button.x = x;
    e->button.y = y;
    e->button.button = 1;
    gboolean handled = FALSE;
    g_signal_emit_by_name(w, "button-press-event", e, &handled);
    e->type = GDK_BUTTON_RELEASE;
    g_signal_emit_by_name(w, "button-release-event", e, &handled);
    gdk_event_free(e);
  };
  host.drag = [](View& view, double x, double from, double to) {
    auto* w = GTK_WIDGET(view.native_handle());
    gboolean handled = FALSE;
    GdkEvent* button = gdk_event_new(GDK_BUTTON_PRESS);
    button->button.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(w)));
    button->button.send_event = TRUE; button->button.x = x; button->button.y = from; button->button.button = 1;
    g_signal_emit_by_name(w, "button-press-event", button, &handled);
    for (int step = 1; step <= 6; ++step) {
      GdkEvent* motion = gdk_event_new(GDK_MOTION_NOTIFY);
      motion->motion.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(w)));
      motion->motion.send_event = TRUE; motion->motion.x = x; motion->motion.y = from + (to - from) * step / 6;
      motion->motion.state = GDK_BUTTON1_MASK;
      g_signal_emit_by_name(w, "motion-notify-event", motion, &handled);
      gdk_event_free(motion);
    }
    button->type = GDK_BUTTON_RELEASE; button->button.y = to;
    g_signal_emit_by_name(w, "button-release-event", button, &handled);
    gdk_event_free(button);
  };
  host.key = [](View& view) {
    auto* w = GTK_WIDGET(view.native_handle());
    GdkEvent* e = gdk_event_new(GDK_KEY_PRESS);
    e->key.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(w)));
    e->key.keyval = GDK_KEY_a;
    gboolean handled = FALSE;
    g_signal_emit_by_name(w, "key-press-event", e, &handled);
    gdk_event_free(e);
  };
  host.window_visible = [window](bool visible) {
    if (visible)
      gtk_widget_show(window);
    else
      gtk_widget_hide(window);
  };
  int result = smoke(pack, host, report);
  gtk_widget_destroy(window);
  while (g_main_context_iteration(nullptr, FALSE)) {
  }
  return result;
}
