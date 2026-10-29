// Private layer-shell checkerboard behind the transparency regression clients.
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
static void draw(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data) {
    for (int y=0;y<height;y+=16) for (int x=0;x<width;x+=16) {
        double c=((x/16+y/16)%2)?0.8:0.2;
        cairo_set_source_rgb(cr,c,c,c);cairo_rectangle(cr,x,y,16,16);cairo_fill(cr);
    }
}
static void activate(GtkApplication* app) {
    GtkWindow* w=GTK_WINDOW(gtk_application_window_new(app));
    gtk_layer_init_for_window(w);gtk_layer_set_namespace(w,"fb20-background");
    gtk_layer_set_layer(w,GTK_LAYER_SHELL_LAYER_BACKGROUND);
    for(int edge=0;edge<4;++edge) gtk_layer_set_anchor(w,edge,TRUE);
    GtkWidget* area=gtk_drawing_area_new();gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area),draw,NULL,NULL);
    gtk_window_set_child(w,area);gtk_window_present(w);
}
int main(int argc,char**argv) {
    GtkApplication* app=gtk_application_new("io.hymission.test.opacitybackground",G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app,"activate",G_CALLBACK(activate),NULL);int result=g_application_run(G_APPLICATION(app),argc,argv);
    g_object_unref(app);return result;
}
