//
// ====================================================================
//
// This implementation uses webkit2gtk backend. It requires gtk+3.0 and
// webkit2gtk-4.0 libraries. Proper compiler flags can be retrieved via:
//
//   pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.0
//
// ====================================================================
//
#include <JavaScriptCore/JavaScript.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

namespace webview {

class gtk_webkit_engine {
  public:
  gtk_webkit_engine(bool debug, void *window)
      : m_window(static_cast<GtkWidget *>(window)) {

    gtk_init_check(0, NULL);
    m_window = static_cast<GtkWidget *>(window);

    if (m_window == nullptr) {
      m_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    }

    g_signal_connect(G_OBJECT(m_window), "destroy",
      G_CALLBACK(+[](GtkWidget *, gpointer arg) {
        static_cast<gtk_webkit_engine *>(arg)->terminate();
        exit(0);
      }),
      this
    );

    // Initialize webview widget
    m_webview = webkit_web_view_new();

    WebKitUserContentManager *manager =
      webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(m_webview));

    g_signal_connect(
      manager,
      "script-message-received::external",
      G_CALLBACK(+[](WebKitUserContentManager *,
      WebKitJavascriptResult *r, gpointer arg) {
        auto *w = static_cast<gtk_webkit_engine *>(arg);
#if WEBKIT_MAJOR_VERSION >= 2 && WEBKIT_MINOR_VERSION >= 22
        JSCValue *value =
          webkit_javascript_result_get_js_value(r);
        char *s = jsc_value_to_string(value);
#else
        JSGlobalContextRef ctx =
          webkit_javascript_result_get_global_context(r);
        JSValueRef value = webkit_javascript_result_get_value(r);
        JSStringRef js = JSValueToStringCopy(ctx, value, NULL);
        size_t n = JSStringGetMaximumUTF8CStringSize(js);
        char *s = g_new(char, n);
        JSStringGetUTF8CString(js, s, n);
        JSStringRelease(js);
#endif
        w->on_message(s);
        g_free(s);
      }),
      this
    );

    webkit_user_content_manager_register_script_message_handler(
      manager,
      "external"
    );

    init(
      "window.external = {"
      "  invoke: s => window.webkit.messageHandlers.external.postMessage(s)"
      "}"
    );

    m_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER (m_window), m_vbox);

    // add the webview to the vertical box
    // gtk_container_add(GTK_CONTAINER(m_window), GTK_WIDGET(m_webview));
    gtk_widget_grab_focus(GTK_WIDGET(m_webview));

    WebKitSettings *settings =
        webkit_web_view_get_settings(WEBKIT_WEB_VIEW(m_webview));
    webkit_settings_set_javascript_can_access_clipboard(settings, true);

    if (debug) {
      webkit_settings_set_enable_write_console_messages_to_stdout(settings, true);
      webkit_settings_set_enable_developer_extras(settings, true);
    }

    gtk_box_pack_end(GTK_BOX (m_vbox), m_webview, TRUE, TRUE, 0);
    gtk_widget_show_all(m_window);
  }

  void menu(std::string menu) {
    if (menu.empty()) return void(0);

    GtkWidget *menubar = gtk_menu_bar_new();

    //
    // TODO(@heapwolf): do a similar loop to the macOS implementation,
    // ---
    //

    // deserialize the menu
    std::replace(menu.begin(), menu.end(), '_', '\n');

    // split on ;
    auto menus = split(menu, ';');

    for (auto m : menus) {
      auto menu = split(m, '\n');
      auto menuTitle = split(trim(menu[0]), ':')[0];
      GtkWidget *subMenu = gtk_menu_new();
      GtkWidget *menuItem = gtk_menu_item_new_with_label(menuTitle.c_str());

      for (int i = 1; i < menu.size(); i++) {
        auto parts = split(trim(menu[i]), ':');
        auto title = parts[0];
        std::string key = "";

        if (parts.size() > 1) {
          key = parts[1] == "_" ? "" : trim(parts[1]);
        }

        GtkWidget *item = gtk_menu_item_new_with_label(title.c_str());
        gtk_widget_set_name(item, menuTitle.c_str());
        gtk_menu_shell_append(GTK_MENU_SHELL(subMenu), item);
        // TODO(@heapwolf): how can we set the accellerator?
        // gtk_accel_group_connect(

        g_signal_connect(
          G_OBJECT(item),
          "activate",
          G_CALLBACK(+[](GtkWidget *t, gpointer arg) {
            auto w = static_cast<webview::gtk_webkit_engine*>(arg);
            auto title = gtk_menu_item_get_label(GTK_MENU_ITEM(t));
            auto parent = gtk_widget_get_name(t);

            // TODO(@heapwolf) can we get the state?
            w->eval(
              "(() => {"
              "  const detail = {"
              "    title: '" + std::string(title) + "',"
              "    parent: '" + std::string(parent) + "',"
              "    state: 0"
              "  };"

              "  const event = new window.CustomEvent('menuItemSelected', { detail });"
              "  window.dispatchEvent(event);"
              "})()"
            );
          }),
          this
        );
      }

      gtk_menu_item_set_submenu(GTK_MENU_ITEM(menuItem), subMenu);
      gtk_menu_shell_append(GTK_MENU_SHELL(menubar), menuItem);
    }

    gtk_box_pack_start(GTK_BOX(m_vbox), menubar, FALSE, FALSE, 0);
    gtk_widget_show_all(m_window);
  }

  void *window() { return (void *)m_window; }

  void run() { gtk_main(); }

  void terminate() { gtk_main_quit(); }

  void dispatch(std::function<void()> f) {
    g_idle_add_full(
      G_PRIORITY_HIGH_IDLE,
      (GSourceFunc)([](void *f) -> int {
        (*static_cast<dispatch_fn_t *>(f))();
        return G_SOURCE_REMOVE;
      }),
    new std::function<void()>(f),
    [](void *f) { delete static_cast<dispatch_fn_t *>(f); }
    );
  }

  void set_title(const std::string title) {
    gtk_window_set_title(GTK_WINDOW(m_window), title.c_str());
  }

  void set_size(int width, int height, int hints) {
    gtk_window_set_resizable(
      GTK_WINDOW(m_window),
      hints != WEBVIEW_HINT_FIXED
    );

    if (hints == WEBVIEW_HINT_NONE) {
      gtk_window_resize(GTK_WINDOW(m_window), width, height);
    } else if (hints == WEBVIEW_HINT_FIXED) {
      gtk_widget_set_size_request(m_window, width, height);
    } else {
      GdkGeometry g;
      g.min_width = g.max_width = width;
      g.min_height = g.max_height = height;
      GdkWindowHints h =
          (hints == WEBVIEW_HINT_MIN ? GDK_HINT_MIN_SIZE : GDK_HINT_MAX_SIZE);
      // This defines either MIN_SIZE, or MAX_SIZE, but not both:
      gtk_window_set_geometry_hints(GTK_WINDOW(m_window), nullptr, &g, h);
    }
  }

  void navigate(const std::string url) {
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(m_webview), url.c_str());
  }

  void init(const std::string js) {
    WebKitUserContentManager *manager =
        webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(m_webview));
    webkit_user_content_manager_add_script(
        manager, webkit_user_script_new(
                     js.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                     WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL));
  }

  void eval(const std::string js) {
    webkit_web_view_run_javascript(
      WEBKIT_WEB_VIEW(m_webview),
      js.c_str(),
      NULL,
      NULL,
      NULL
    );
  }

  GtkWidget *m_vbox;
  GtkWidget *m_window;
  std::string lastMenuSelection;
private:
  virtual void on_message(const std::string msg) = 0;
  GtkWidget *m_webview;
};

using browser_engine = gtk_webkit_engine;
} // namespace webview
