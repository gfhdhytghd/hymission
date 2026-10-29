// Private isolated protocol regression client; never connects to host IPC.
#include <wayland-client.h>
#include "ext-workspace-client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct workspace { struct ext_workspace_handle_v1* handle; char* name; };
static struct workspace workspaces[256];
static size_t count;
static struct ext_workspace_manager_v1* manager;
static void text_id(void* d,struct ext_workspace_handle_v1* w,const char* id) { (void)d;(void)w;(void)id; }
static void name(void* d,struct ext_workspace_handle_v1* w,const char* text) { (void)w; struct workspace* ws=d; free(ws->name); ws->name=strdup(text); }
static void coordinates(void* d,struct ext_workspace_handle_v1* w,struct wl_array* a) { (void)d;(void)w;(void)a; }
static void state(void* d,struct ext_workspace_handle_v1* w,uint32_t flags) { (void)d;(void)w;(void)flags; }
static void removed(void* d,struct ext_workspace_handle_v1* w) { (void)d;(void)w; }
static const struct ext_workspace_handle_v1_listener workspace_listener={text_id,name,coordinates,state,state,removed};
static void group_caps(void* d,struct ext_workspace_group_handle_v1* g,uint32_t flags) { (void)d;(void)g;(void)flags; }
static void group_output(void* d,struct ext_workspace_group_handle_v1* g,struct wl_output* o) { (void)d;(void)g;(void)o; }
static void group_workspace(void* d,struct ext_workspace_group_handle_v1* g,struct ext_workspace_handle_v1* w) { (void)d;(void)g;(void)w; }
static void group_removed(void* d,struct ext_workspace_group_handle_v1* g) { (void)d;(void)g; }
static const struct ext_workspace_group_handle_v1_listener group_listener={group_caps,group_output,group_output,group_workspace,group_workspace,group_removed};
static void group(void* d,struct ext_workspace_manager_v1* m,struct ext_workspace_group_handle_v1* g) { (void)d;(void)m; ext_workspace_group_handle_v1_add_listener(g,&group_listener,NULL); }
static void workspace(void* d,struct ext_workspace_manager_v1* m,struct ext_workspace_handle_v1* w) {
    (void)d;(void)m;
    if (count==256) abort();
    struct workspace* ws=&workspaces[count++]; ws->handle=w;
    ext_workspace_handle_v1_add_listener(w,&workspace_listener,ws);
}
static void done(void* d,struct ext_workspace_manager_v1* m) { (void)d;(void)m; }
static const struct ext_workspace_manager_v1_listener manager_listener={group,workspace,done,done};
static void global(void* d,struct wl_registry* r,uint32_t id,const char* interface,uint32_t version) {
    (void)d;(void)version;
    if (!strcmp(interface,ext_workspace_manager_v1_interface.name)) {
        manager=wl_registry_bind(r,id,&ext_workspace_manager_v1_interface,1);
        ext_workspace_manager_v1_add_listener(manager,&manager_listener,NULL);
    }
}
static void global_removed(void* d,struct wl_registry* r,uint32_t id) { (void)d;(void)r;(void)id; }
static const struct wl_registry_listener registry_listener={global,global_removed};
int main(int argc,char** argv) {
    if (argc!=2) return 2;
    struct wl_display* display=wl_display_connect(NULL);
    if (!display) return 3;
    struct wl_registry* registry=wl_display_get_registry(display);
    wl_registry_add_listener(registry,&registry_listener,NULL);
    for (int i=0;i<3;++i) if (wl_display_roundtrip(display)<0) return 4;
    if (!manager) return 5;
    struct workspace* target=NULL;
    for (size_t i=0;i<count;++i) if (workspaces[i].name && !strcmp(workspaces[i].name,argv[1])) target=&workspaces[i];
    if (!target) { fprintf(stderr,"workspace not advertised: %s\n",argv[1]); return 6; }
    ext_workspace_handle_v1_activate(target->handle);
    ext_workspace_manager_v1_commit(manager);
    if (wl_display_roundtrip(display)<0) return 7;
    wl_display_disconnect(display);
    return 0;
}
