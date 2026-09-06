#include <gtk/gtk.h>
#include <adwaita.h>
#include <gtk4-layer-shell.h>

#include <array>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr int IPC_FD = 3;

struct AppState {
    GtkWindow*     window = nullptr;
    GtkLabel*      queryLabel = nullptr;
    GtkLabel*      countLabel = nullptr;
    GtkIMContext*  im = nullptr;
    std::string    query;
    std::string    preedit;
    std::size_t    cursor = 0;
};

bool sendPacket(char type, const std::string& payload = {}) {
    std::string packet(1, type);
    packet += payload;
    return send(IPC_FD, packet.data(), packet.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(packet.size());
}

void updateLabel(AppState* state) {
    std::string shown = state->query;
    if (!state->preedit.empty())
        shown.insert(state->cursor, state->preedit);
    gtk_label_set_text(state->queryLabel, shown.empty() ? "Search windows" : shown.c_str());
}

void publishQuery(AppState* state) {
    updateLabel(state);
    if (!sendPacket('Q', state->query))
        g_application_quit(g_application_get_default());
}

void commitText(GtkIMContext*, const char* text, gpointer data) {
    auto* state = static_cast<AppState*>(data);
    state->query.insert(state->cursor, text);
    state->cursor += std::strlen(text);
    state->preedit.clear();
    publishQuery(state);
}

void preeditChanged(GtkIMContext* context, gpointer data) {
    auto* state = static_cast<AppState*>(data);
    gchar* text = nullptr;
    gint cursor = 0;
    gtk_im_context_get_preedit_string(context, &text, nullptr, &cursor);
    state->preedit = text ? text : "";
    g_free(text);
    updateLabel(state);
    sendPacket('P', state->preedit.empty() ? "0" : "1");
}

void erasePreviousCodepoint(AppState* state) {
    if (state->cursor == 0)
        return;
    const char* begin = state->query.data();
    const char* previous = g_utf8_find_prev_char(begin, begin + state->cursor);
    if (!previous)
        return;
    const auto offset = static_cast<std::size_t>(previous - begin);
    state->query.erase(offset, state->cursor - offset);
    state->cursor = offset;
    publishQuery(state);
}

gboolean keyPressed(GtkEventControllerKey* controller, guint keyval, guint, GdkModifierType modifiers, gpointer data) {
    auto* state = static_cast<AppState*>(data);
    if (GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller)); event && gtk_im_context_filter_keypress(state->im, event))
        return TRUE;

    if (modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK))
        return FALSE;

    switch (keyval) {
        case GDK_KEY_BackSpace:
            erasePreviousCodepoint(state);
            return TRUE;
        case GDK_KEY_Delete:
            if (state->cursor < state->query.size()) {
                const char* begin = state->query.data();
                const char* next = g_utf8_next_char(begin + state->cursor);
                state->query.erase(state->cursor, static_cast<std::size_t>(next - (begin + state->cursor)));
                publishQuery(state);
            }
            return TRUE;
        case GDK_KEY_Left:
            if (state->cursor > 0) {
                const char* previous = g_utf8_find_prev_char(state->query.data(), state->query.data() + state->cursor);
                if (previous)
                    state->cursor = static_cast<std::size_t>(previous - state->query.data());
            }
            return TRUE;
        case GDK_KEY_Right:
            if (state->cursor < state->query.size())
                state->cursor = static_cast<std::size_t>(g_utf8_next_char(state->query.data() + state->cursor) - state->query.data());
            return TRUE;
        case GDK_KEY_Up:
            return sendPacket('N', "-1") ? TRUE : FALSE;
        case GDK_KEY_Down:
            return sendPacket('N', "1") ? TRUE : FALSE;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
            return sendPacket('A') ? TRUE : FALSE;
        case GDK_KEY_Escape:
            return sendPacket('E') ? TRUE : FALSE;
        default:
            break;
    }

    const gunichar ch = gdk_keyval_to_unicode(keyval);
    if (ch != 0 && !g_unichar_iscntrl(ch)) {
        char utf8[7] = {};
        const int length = g_unichar_to_utf8(ch, utf8);
        state->query.insert(state->cursor, utf8, static_cast<std::size_t>(length));
        state->cursor += static_cast<std::size_t>(length);
        publishQuery(state);
        return TRUE;
    }
    return FALSE;
}

gboolean ipcReady(GIOChannel* channel, GIOCondition condition, gpointer data) {
    auto* state = static_cast<AppState*>(data);
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        g_application_quit(g_application_get_default());
        return G_SOURCE_REMOVE;
    }
    std::array<char, 256> packet{};
    const ssize_t size = recv(g_io_channel_unix_get_fd(channel), packet.data(), packet.size() - 1, 0);
    if (size <= 0) {
        g_application_quit(g_application_get_default());
        return G_SOURCE_REMOVE;
    }
    if (packet[0] == 'C')
        gtk_label_set_text(state->countLabel, (std::string(packet.data() + 1, static_cast<std::size_t>(size - 1)) + " results").c_str());
    return G_SOURCE_CONTINUE;
}

void activate(GtkApplication* app, gpointer data) {
    auto* state = static_cast<AppState*>(data);
    state->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_widget_add_css_class(GTK_WIDGET(state->window), "hymission-search");
    gtk_window_set_decorated(state->window, FALSE);
    gtk_layer_init_for_window(state->window);
    gtk_layer_set_layer(state->window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(state->window, "hymission-search");
    gtk_layer_set_anchor(state->window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_margin(state->window, GTK_LAYER_SHELL_EDGE_TOP, 28);
    gtk_layer_set_keyboard_mode(state->window, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(box, "searchbar");
    state->queryLabel = GTK_LABEL(gtk_label_new("Search windows"));
    gtk_label_set_xalign(state->queryLabel, 0.0F);
    gtk_widget_set_size_request(GTK_WIDGET(state->queryLabel), 360, -1);
    state->countLabel = GTK_LABEL(gtk_label_new(""));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->queryLabel));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->countLabel));
    gtk_window_set_child(state->window, box);

    auto* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        "window.hymission-search { background: transparent; box-shadow: none; }"
        ".searchbar { background: @window_bg_color; color: @window_fg_color; border-radius: 8px; padding: 12px 16px; margin: 12px; box-shadow: 0 3px 10px alpha(black,0.18); }"
        ".searchbar label:last-child { opacity: 0.62; }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(GTK_WIDGET(state->window)), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    state->im = gtk_im_multicontext_new();
    gtk_im_context_set_client_widget(state->im, GTK_WIDGET(state->window));
    g_signal_connect(state->im, "commit", G_CALLBACK(commitText), state);
    g_signal_connect(state->im, "preedit-changed", G_CALLBACK(preeditChanged), state);
    gtk_im_context_focus_in(state->im);

    auto* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(keyPressed), state);
    gtk_widget_add_controller(GTK_WIDGET(state->window), keys);
    gtk_widget_set_focusable(GTK_WIDGET(state->window), TRUE);
    // GTK can skip the initial buffer for a fully transparent window, leaving
    // layer-shell unmapped and unable to receive the key that would reveal it.
    gtk_window_present(state->window);
    gtk_widget_grab_focus(GTK_WIDGET(state->window));

    GIOChannel* channel = g_io_channel_unix_new(IPC_FD);
    g_io_channel_set_encoding(channel, nullptr, nullptr);
    g_io_add_watch(channel, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL), ipcReady, state);
    g_io_channel_unref(channel);
    sendPacket('R');
}
} // namespace

int main(int argc, char** argv) {
    AppState state;
    GtkApplication* app = GTK_APPLICATION(adw_application_new("io.github.wilf.hymission.search", G_APPLICATION_NON_UNIQUE));
    g_signal_connect(app, "activate", G_CALLBACK(activate), &state);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    if (state.im) {
        gtk_im_context_focus_out(state.im);
        g_object_unref(state.im);
    }
    g_object_unref(app);
    return status;
}
