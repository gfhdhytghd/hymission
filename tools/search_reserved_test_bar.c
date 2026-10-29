// Private nested-session fixture: a real layer-shell exclusive top bar.
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
static void activate(GtkApplication* app) {
    GtkWindow* window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_layer_init_for_window(window);
    gtk_layer_set_namespace(window, "hymission-test-exclusive-bar");
    gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_exclusive_zone(window, 48);
    gtk_widget_set_size_request(GTK_WIDGET(window), -1, 48);
    gtk_window_set_child(window, gtk_label_new("Exclusive top bar: 48 logical pixels"));
    gtk_window_present(window);
}
int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("io.hymission.test.exclusivebar", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
