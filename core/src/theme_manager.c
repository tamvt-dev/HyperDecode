#include "../include/theme_manager.h"
#include "../include/logging.h"
#include <gtk/gtk.h>
#include <glib.h>
#include <stdio.h>

static ThemeType current_theme = THEME_DARK;
static GtkCssProvider *css_provider = NULL;
static char *config_path = NULL;

static ThemeType theme_manager_resolve(ThemeType theme) {
    if (theme == THEME_SYSTEM) {
        return THEME_DARK;
    }
    return theme;
}

void theme_manager_init(void) {
    char *config_dir = g_build_filename(g_get_user_config_dir(), "hyperdecode", NULL);
    g_mkdir_with_parents(config_dir, 0755);
    g_free(config_dir);
    
    config_path = g_build_filename(g_get_user_config_dir(), "hyperdecode", "theme.conf", NULL);
    theme_manager_load();
    log_info("Theme manager initialized");
}

void theme_manager_apply(void *widget, ThemeType theme) {
    current_theme = theme;
    ThemeType resolved_theme = theme_manager_resolve(theme);
    
    if (css_provider) {
        gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css_provider));
        g_object_unref(css_provider);
    }
    css_provider = gtk_css_provider_new();
    
    const char *css_data;
    if (resolved_theme == THEME_LIGHT) {
        css_data =
            "window, .window, .background {\n"
            "    background-color: #F9F9F9;\n"
            "}\n"
            "button {\n"
            "    background-color: #E8E8E8;\n"
            "    color: #0A0E27;\n"
            "    border: 1px solid #D0D0D0;\n"
            "    border-radius: 4px;\n"
            "    padding: 6px 12px;\n"
            "}\n"
            "button:hover {\n"
            "    background-color: #DCDCDC;\n"
            "    border-color: #006A9A;\n"
            "    color: #006A9A;\n"
            "}\n"
            "button:checked, button:active {\n"
            "    background-color: #1A7D05;\n"
            "    color: #FFFFFF;\n"
            "    border-color: #1A7D05;\n"
            "}\n"
            "entry {\n"
            "    background-color: #ffffff;\n"
            "    color: #0A0E27;\n"
            "    border: 1px solid #D0D0D0;\n"
            "    border-radius: 4px;\n"
            "    padding: 6px;\n"
            "}\n"
            "entry:focus {\n"
            "    border-color: #1A7D05;\n"
            "}\n"
            "textview {\n"
            "    background-color: #ffffff;\n"
            "}\n"
            "textview text {\n"
            "    background-color: #ffffff;\n"
            "    color: #0A0E27;\n"
            "}\n"
            "label {\n"
            "    color: #0A0E27;\n"
            "}\n"
            "notebook tab {\n"
            "    background-color: #E8E8E8;\n"
            "    color: #555555;\n"
            "    padding: 6px 12px;\n"
            "}\n"
            "notebook tab:checked {\n"
            "    background-color: #ffffff;\n"
            "    color: #1A7D05;\n"
            "}\n"
            "statusbar {\n"
            "    background-color: #FFFFFF;\n"
            "    color: #0A0E27;\n"
            "}";
    } else {
        css_data =
            "window, .window, .background {\n"
            "    background-color: #0A0E27;\n"
            "}\n"
            "button {\n"
            "    background-color: #252D4A;\n"
            "    color: #E7E5E4;\n"
            "    border: 1px solid #303850;\n"
            "    border-radius: 4px;\n"
            "    padding: 6px 12px;\n"
            "}\n"
            "button:hover {\n"
            "    background-color: #303850;\n"
            "    border-color: #00A6F4;\n"
            "    color: #00A6F4;\n"
            "}\n"
            "button:checked, button:active {\n"
            "    background-color: #37F712;\n"
            "    color: #0A0E27;\n"
            "    border-color: #37F712;\n"
            "}\n"
            "entry {\n"
            "    background-color: #10162F;\n"
            "    color: #E7E5E4;\n"
            "    border: 1px solid #252D4A;\n"
            "    border-radius: 4px;\n"
            "    padding: 6px;\n"
            "}\n"
            "entry:focus {\n"
            "    border-color: #37F712;\n"
            "}\n"
            "textview {\n"
            "    background-color: #10162F;\n"
            "}\n"
            "textview text {\n"
            "    background-color: #10162F;\n"
            "    color: #E7E5E4;\n"
            "}\n"
            "label {\n"
            "    color: #E7E5E4;\n"
            "}\n"
            "notebook tab {\n"
            "    background-color: #141933;\n"
            "    color: #A99F9B;\n"
            "    padding: 6px 12px;\n"
            "}\n"
            "notebook tab:checked {\n"
            "    background-color: #0A0E27;\n"
            "    color: #37F712;\n"
            "}\n"
            "statusbar {\n"
            "    background-color: #141933;\n"
            "    color: #E7E5E4;\n"
            "}";
    }
    
    GError *error = NULL;
    gtk_css_provider_load_from_data(css_provider, css_data, -1, &error);
    if (error) {
        log_error("CSS error: %s", error->message);
        g_error_free(error);
        return;
    }
    
    GdkScreen *screen = gdk_screen_get_default();
    if (screen) {
        gtk_style_context_add_provider_for_screen(screen,
            GTK_STYLE_PROVIDER(css_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    
    log_info("Theme applied: %s", resolved_theme == THEME_DARK ? "Dark" : "Light");
    
    if (widget && GTK_IS_WIDGET(widget)) {
        gtk_widget_queue_draw(widget);
    }
    
    GList *windows = gtk_window_list_toplevels();
    for (GList *iter = windows; iter; iter = iter->next) {
        if (GTK_IS_WINDOW(iter->data)) {
            gtk_widget_queue_draw(GTK_WIDGET(iter->data));
        }
    }
    g_list_free(windows);
}

void theme_manager_toggle(void *widget) {
    if (current_theme == THEME_DARK) {
        theme_manager_apply(widget, THEME_LIGHT);
    } else {
        theme_manager_apply(widget, THEME_DARK);
    }
    theme_manager_save();
}

ThemeType theme_manager_get_current(void) {
    return current_theme;
}

void theme_manager_save(void) {
    GKeyFile *keyfile = g_key_file_new();
    g_key_file_set_integer(keyfile, "Theme", "current", current_theme);
    gsize data_len;
    char *data = g_key_file_to_data(keyfile, &data_len, NULL);
    g_file_set_contents(config_path, data, data_len, NULL);
    g_free(data);
    g_key_file_free(keyfile);
    log_info("Theme saved: %d", current_theme);
}

void theme_manager_save_with_value(ThemeType theme) {
    current_theme = theme;
    theme_manager_save();
}

void theme_manager_load(void) {
    GKeyFile *keyfile = g_key_file_new();
    if (g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, NULL)) {
        current_theme = g_key_file_get_integer(keyfile, "Theme", "current", NULL);
        log_info("Theme loaded: %d", current_theme);
    } else {
        current_theme = THEME_DARK;
        log_info("Using default theme: Dark");
    }
    g_key_file_free(keyfile);
}
   
