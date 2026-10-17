// Private headless test parent only. Weston headless has no seat by default,
// but Aquamarine's nested backend requires wl_seat even without input devices.
#include <libweston/libweston.h>
#include <stdlib.h>
extern void weston_seat_init(struct weston_seat*, struct weston_compositor*, const char*);
extern int weston_seat_init_pointer(struct weston_seat*);
extern int weston_seat_init_keyboard(struct weston_seat*, struct xkb_keymap*);
WL_EXPORT int wet_module_init(struct weston_compositor* compositor, int* argc, char* argv[]) {
    struct weston_seat* seat = calloc(1, sizeof(*seat));
    if (!seat) return -1;
    weston_seat_init(seat, compositor, "tiles-test-seat");
    if (weston_seat_init_pointer(seat) < 0 || weston_seat_init_keyboard(seat, NULL) < 0) return -1;
    return 0;
}
