// Build with -O2: forwarding must tail-call libc dlsym so RTLD_NEXT retains
// the original caller (verified on this glibc x86-64 test environment).
// Isolated regression only: trigger findFunctionsByName's failed-dlsym path
// before the swipe update symbol. Never preloaded in the host or real clients.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
void* dlsym(void* handle, const char* symbol) {
    typedef void* (*Lookup)(void*, const char*);
    static Lookup original;
    if (!original) original=(Lookup)dlvsym(RTLD_NEXT,"dlsym","GLIBC_2.2.5");
    if (strcmp(symbol,"_ZN10CDMABuffer6updateERKN9Hyprutils4Math7CRegionE")==0) return 0;
    return original(handle,symbol);
}
