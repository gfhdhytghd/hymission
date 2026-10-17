// Test-only: the local Aquamarine Wayland backend unconditionally binds
// wl_compositor v6, while the private Weston 15 parent advertises v5. Clamp
// that one parent protocol binding. No compositor/plugin code is patched.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <wayland-client-core.h>
struct wl_proxy* wl_proxy_marshal_array_flags(struct wl_proxy* proxy, uint32_t opcode,
        const struct wl_interface* interface, uint32_t version, uint32_t flags, union wl_argument* args) {
    typedef struct wl_proxy* (*Fn)(struct wl_proxy*, uint32_t, const struct wl_interface*, uint32_t, uint32_t, union wl_argument*);
    static Fn original;
    if (!original) original = (Fn)dlsym(RTLD_NEXT, "wl_proxy_marshal_array_flags");
    if (interface && strcmp(wl_proxy_get_class(proxy), "wl_registry") == 0 && opcode == 0) {
        uint32_t cap = version;
        if (strcmp(interface->name,"wl_compositor")==0 || strcmp(interface->name,"xdg_wm_base")==0) cap=5;
        if (strcmp(interface->name,"wl_seat")==0) cap=7;
        if (version>cap) { args[2].u=cap; version=cap; }
    }
    return original(proxy, opcode, interface, version, flags, args);
}
