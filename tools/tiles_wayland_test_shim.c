// Test-only: the local Aquamarine Wayland backend unconditionally binds
// wl_compositor v6, while the private Weston 15 parent advertises v5. Clamp
// that one parent protocol binding. No compositor/plugin code is patched.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <wayland-client-core.h>
// Opt-in workaround for the stock nested backend's initial configure order.
// Keep this on the private parent connection: delay the toplevel size callback
// until the native xdg_surface callback has sent its ack. Rendering is untouched.
static void (*top_configure)(void*,struct wl_proxy*,int32_t,int32_t,struct wl_array*);
static void (*surface_configure)(void*,struct wl_proxy*,uint32_t);
static struct { void* data; struct wl_proxy* proxy; int32_t w,h; struct wl_array states; } pending;
static void delayed_top(void* data,struct wl_proxy* proxy,int32_t w,int32_t h,struct wl_array* states) {
    pending.data=data;pending.proxy=proxy;pending.w=w;pending.h=h;
    wl_array_init(&pending.states);
    if (states->size) memcpy(wl_array_add(&pending.states,states->size),states->data,states->size);
}
static void acknowledged_surface(void* data,struct wl_proxy* proxy,uint32_t serial) {
    surface_configure(data,proxy,serial);
    if (pending.proxy) {
        struct wl_proxy* top=pending.proxy;pending.proxy=NULL;
        top_configure(pending.data,top,pending.w,pending.h,&pending.states);
        wl_array_release(&pending.states);
    }
}
int wl_proxy_add_listener(struct wl_proxy* proxy,void (**implementation)(void),void* data) {
    typedef int (*Fn)(struct wl_proxy*,void (**)(void),void*);
    static Fn original;
    static void (*top[4])(void),(*surface[1])(void);
    if (!original) original=(Fn)dlsym(RTLD_NEXT,"wl_proxy_add_listener");
    if (getenv("HM_TEST_CONFIGURE_ORDER")) {
        const char* name=wl_proxy_get_class(proxy);
        if (strcmp(name,"xdg_toplevel")==0) {
            memcpy(top,implementation,sizeof(top));top_configure=(void*)top[0];top[0]=(void*)delayed_top;
            return original(proxy,top,data);
        }
        if (strcmp(name,"xdg_surface")==0) {
            surface_configure=(void*)implementation[0];surface[0]=(void*)acknowledged_surface;
            return original(proxy,surface,data);
        }
    }
    return original(proxy,implementation,data);
}
struct wl_proxy* wl_proxy_marshal_array_flags(struct wl_proxy* proxy, uint32_t opcode,
        const struct wl_interface* interface, uint32_t version, uint32_t flags, union wl_argument* args) {
    typedef struct wl_proxy* (*Fn)(struct wl_proxy*, uint32_t, const struct wl_interface*, uint32_t, uint32_t, union wl_argument*);
    static Fn original;
    if (!original) original = (Fn)dlsym(RTLD_NEXT, "wl_proxy_marshal_array_flags");
    static struct wl_proxy* initial_surface;
    if (getenv("HM_TEST_CONFIGURE_ORDER") && interface && strcmp(interface->name,"xdg_surface")==0)
        initial_surface=(struct wl_proxy*)args[1].o;
    if (proxy==initial_surface && opcode==1 && args[0].o) {
        initial_surface=NULL;
        wl_display_roundtrip(wl_proxy_get_display(proxy));
    }
    if (interface && strcmp(wl_proxy_get_class(proxy), "wl_registry") == 0 && opcode == 0) {
        uint32_t cap = version;
        if (strcmp(interface->name,"wl_compositor")==0 || strcmp(interface->name,"xdg_wm_base")==0) cap=5;
        if (strcmp(interface->name,"wl_seat")==0) cap=7;
        if (version>cap) { args[2].u=cap; version=cap; }
    }
    return original(proxy, opcode, interface, version, flags, args);
}
