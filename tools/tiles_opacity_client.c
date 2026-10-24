// Private Wayland regression client with a constant premultiplied alpha surface.
#define _GNU_SOURCE
#include <wayland-client.h>
#include "xdg-shell-client.h"
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* shell;
static struct wl_surface* surface;
static struct xdg_surface* xsurface;
static int width=640,height=480,running=1;
struct Buffer { struct wl_buffer* buffer; void* pixels; size_t bytes; };
static void released(void* data,struct wl_buffer* buffer) {
    struct Buffer* b=data; wl_buffer_destroy(buffer); munmap(b->pixels,b->bytes); free(b);
}
static const struct wl_buffer_listener buffer_listener={released};
static void paint(void) {
    const int header=0,stride=width*4,total=height+header;
    struct Buffer* b=calloc(1,sizeof(*b)); b->bytes=(size_t)stride*total;
    int fd=memfd_create("tiles-opacity",MFD_CLOEXEC);
    if (fd<0 || ftruncate(fd,b->bytes)<0) exit(2);
    b->pixels=mmap(NULL,b->bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if (b->pixels==MAP_FAILED) exit(2);
    uint32_t* pixels=b->pixels;
    const char* alphaText=getenv("FB20_ALPHA");
    const unsigned alpha=alphaText?(unsigned)strtoul(alphaText,NULL,10):255;
    const uint32_t body=(alpha<<24)|((240*alpha/255)<<16)|((160*alpha/255)<<8)|(64*alpha/255);
    for (int y=0;y<total;++y) for(int x=0;x<width;++x) pixels[y*width+x]=y<header?0xffff0000:body;
    struct wl_shm_pool* pool=wl_shm_create_pool(shm,fd,b->bytes);
    b->buffer=wl_shm_pool_create_buffer(pool,0,width,total,stride,WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool); close(fd);
    wl_buffer_add_listener(b->buffer,&buffer_listener,b);
    xdg_surface_set_window_geometry(xsurface,0,header,width,height);
    if (alpha==255) {
        struct wl_region* opaque=wl_compositor_create_region(compositor);
        wl_region_add(opaque,0,0,width,total);wl_surface_set_opaque_region(surface,opaque);wl_region_destroy(opaque);
    }
    wl_surface_attach(surface,b->buffer,0,0);
    wl_surface_damage_buffer(surface,0,0,INT_MAX,INT_MAX);
    wl_surface_commit(surface);
}
static void configured(void* data,struct xdg_surface* s,uint32_t serial) { xdg_surface_ack_configure(s,serial); paint(); }
static const struct xdg_surface_listener surface_listener={configured};
static void resized(void* data,struct xdg_toplevel* top,int32_t w,int32_t h,struct wl_array* states) {
    if(w>0) width=w; if(h>0) height=h;
}
static void closed(void* data,struct xdg_toplevel* top) { running=0; }
static const struct xdg_toplevel_listener top_listener={.configure=resized,.close=closed};
static void ping(void* data,struct xdg_wm_base* s,uint32_t serial) { xdg_wm_base_pong(s,serial); }
static const struct xdg_wm_base_listener shell_listener={ping};
static void global(void* data,struct wl_registry* r,uint32_t name,const char* interface,uint32_t version) {
    if(!strcmp(interface,"wl_compositor")) compositor=wl_registry_bind(r,name,&wl_compositor_interface,4);
    else if(!strcmp(interface,"wl_shm")) shm=wl_registry_bind(r,name,&wl_shm_interface,1);
    else if(!strcmp(interface,"xdg_wm_base")) { shell=wl_registry_bind(r,name,&xdg_wm_base_interface,1); xdg_wm_base_add_listener(shell,&shell_listener,NULL); }
}
static void removed(void* data,struct wl_registry* r,uint32_t name) {}
static const struct wl_registry_listener registry_listener={global,removed};
int main(void) {
    display=wl_display_connect(NULL); if(!display) return 2;
    struct wl_registry* registry=wl_display_get_registry(display);
    wl_registry_add_listener(registry,&registry_listener,NULL); wl_display_roundtrip(display);
    if(!compositor || !shm || !shell) return 2;
    surface=wl_compositor_create_surface(compositor);
    xsurface=xdg_wm_base_get_xdg_surface(shell,surface); xdg_surface_add_listener(xsurface,&surface_listener,NULL);
    struct xdg_toplevel* top=xdg_surface_get_toplevel(xsurface); xdg_toplevel_add_listener(top,&top_listener,NULL);
    const char* id=getenv("FB20_ID");if(!id)id="fb20-client";
    xdg_toplevel_set_title(top,id); xdg_toplevel_set_app_id(top,id);
    wl_surface_commit(surface);
    while(running && wl_display_dispatch(display)>=0) {}
    return 0;
}
