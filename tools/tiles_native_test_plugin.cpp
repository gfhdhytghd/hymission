#include "../src/hook_symbols.hpp"
#include <fstream>
#include "../src/state_json.hpp"
#include "../src/workspace_key.hpp"
// Isolated-test fixture only. Not installed and never loaded in the host session.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <linux/input-event-codes.h>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/workspace/Resolver.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include <chrono>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
namespace {
// Private every-rendered-frame pixel recorder. Readback is deliberately excluded
// from the performance profiler and is never enabled in a host session.
struct CapturedFrame { int width,height; std::vector<unsigned char> pixels; nlohmann::json state; };
bool recordingFrames=false;
std::string captureMonitor;
CHyprSignalListener captureDamageListener;
std::vector<CapturedFrame> capturedFrames;
template<class Framebuffer>
bool readFramePixels(Framebuffer& fb,std::vector<uint8_t>& bytes) {
    const bool fp16=fb.getTexture() && fb.getTexture()->m_drmFormat==0x48344241;
    // The HDR tree's RGBA8 readback rejects FP16 attachments. Resolve a private
    // normalized copy instead of crashing the nested compositor while recording.
    if (fp16) { // DRM_FORMAT_ABGR16161616F
        GLint read=0,draw=0,texture=0,pack=0,alignment=0,rowLength=0,skipRows=0,skipPixels=0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&pack);
        glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);glGetIntegerv(GL_PACK_ROW_LENGTH,&rowLength);
        glGetIntegerv(GL_PACK_SKIP_ROWS,&skipRows);glGetIntegerv(GL_PACK_SKIP_PIXELS,&skipPixels);
        GLuint resolved=0,target=0;glGenFramebuffers(1,&resolved);glGenTextures(1,&target);
        glBindTexture(GL_TEXTURE_2D,target);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,fb.m_size.x,fb.m_size.y,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,resolved);glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
        const GLenum attachment=GL_COLOR_ATTACHMENT0;glDrawBuffers(1,&attachment);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,fb.getFBID());
        glBlitFramebuffer(0,0,fb.m_size.x,fb.m_size.y,0,0,fb.m_size.x,fb.m_size.y,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,resolved);glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glPixelStorei(GL_PACK_ALIGNMENT,1);
        glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);
        glReadPixels(0,0,fb.m_size.x,fb.m_size.y,GL_RGBA,GL_UNSIGNED_BYTE,bytes.data());
        const bool ok=glGetError()==GL_NO_ERROR;
        glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw);
        glBindTexture(GL_TEXTURE_2D,texture);glBindBuffer(GL_PIXEL_PACK_BUFFER,pack);glPixelStorei(GL_PACK_ALIGNMENT,alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH,rowLength);glPixelStorei(GL_PACK_SKIP_ROWS,skipRows);glPixelStorei(GL_PACK_SKIP_PIXELS,skipPixels);
        glDeleteFramebuffers(1,&resolved);glDeleteTextures(1,&target);
        if (ok) return true;
    }
    if constexpr (requires { fb.readPixelsRGBA8(bytes); }) {
        if (!fp16) return fb.readPixelsRGBA8(bytes);
    }
    {
        // Upstream main lacks the HDR tree's convenience readback API. Keep
        // this fixture buildable there, preserving all pack/read bindings.
        GLint readFB=0,alignment=0,rowLength=0,skipRows=0,skipPixels=0,buffer=0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&readFB);glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH,&rowLength);glGetIntegerv(GL_PACK_SKIP_ROWS,&skipRows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS,&skipPixels);glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&buffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,fb.getFBID());glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
        glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ROW_LENGTH,0);
        glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);
        if (fp16) {
            // GLES requires a float read type for a floating attachment. Keep
            // this SDR marker diagnostic linear; do not claim HDR output parity.
            std::vector<float> linear(bytes.size());
            glReadPixels(0,0,fb.m_size.x,fb.m_size.y,GL_RGBA,GL_FLOAT,linear.data());
            for (size_t i=0;i<bytes.size();++i) bytes[i]=std::lround(std::clamp(linear[i],0.F,1.F)*255.F);
        } else glReadPixels(0,0,fb.m_size.x,fb.m_size.y,GL_RGBA,GL_UNSIGNED_BYTE,bytes.data());
        const bool ok=glGetError()==GL_NO_ERROR;
        glBindFramebuffer(GL_READ_FRAMEBUFFER,readFB);glBindBuffer(GL_PIXEL_PACK_BUFFER,buffer);
        glPixelStorei(GL_PACK_ALIGNMENT,alignment);glPixelStorei(GL_PACK_ROW_LENGTH,rowLength);
        glPixelStorei(GL_PACK_SKIP_ROWS,skipRows);glPixelStorei(GL_PACK_SKIP_PIXELS,skipPixels);
        return ok;
    }
}
nlohmann::json nativeLayoutState() {
    nlohmann::json result=nlohmann::json::array();
    const auto box=[](const CBox& b) { return nlohmann::json{{"x",b.x},{"y",b.y},{"width",b.w},{"height",b.h}}; };
    for (const auto& ws:State::workspaceState()->workspacesCopy()) {
        const auto space=ws->space();
        if (!space || !space->algorithm()) continue;
        nlohmann::json item{{"workspace",hymission::workspaceKey(ws)},{"work_area",box(space->workArea())}};
        auto* scrolling=dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(space->algorithm()->tiledAlgo().get());
        SP<Layout::Tiled::SScrollingData> data;
        if (scrolling) for (const auto& window:Desktop::windowState()->windows()) {
            if (!window->mapped() || window->m_workspace!=ws || window->isFloating()) continue;
            const auto target=scrolling->dataFor(window->layoutTarget(),true);
            const auto column=target?target->column.lock():nullptr;
            if (column) { data=column->scrollingData.lock(); break; }
        }
        if (scrolling && data && data->controller) {
            item["usable_area"]=box(scrolling->usableArea());
            item["offset"]=data->controller->getOffset();
            item["columns"]=nlohmann::json::array();
            for (const auto& column:data->columns) {
                nlohmann::json targets=nlohmann::json::array();
                for (const auto& data:column->targetDatas) targets.push_back(box(data->layoutBox));
                item["columns"].push_back({{"fraction",column->getColumnWidth()},{"targets",targets}});
            }
        }
        result.push_back(std::move(item));
    }
    return result;
}
int layoutState(lua_State* L) {
    std::ofstream output(luaL_checkstring(L,1)); output<<nativeLayoutState().dump(2);return 0;
}
class CaptureFramePass final:public IPassElement {
 public:
    std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) override {
        if (!recordingFrames || capturedFrames.size()>=180 || !ctx.m_data.currentFB ||
            (!captureMonitor.empty() && ctx.m_data.pMonitor->m_name!=captureMonitor)) return {};
        const auto fb=dynamicPointerCast<Render::GL::CGLFramebuffer>(ctx.m_data.currentFB);
        if (!fb) throw std::runtime_error("private capture requires native GL framebuffer");
        const auto size=fb->m_size;
        CapturedFrame frame{static_cast<int>(size.x),static_cast<int>(size.y),{}, {}};
        // Sample animation phase before synchronous GPU readback advances time.
        const auto stageBeforeReadback=nlohmann::json::parse(HyprlandAPI::invokeHyprctlCommand("hymission-stage-state","","j"));
        const auto overviewBeforeReadback=nlohmann::json::parse(HyprlandAPI::invokeHyprctlCommand("hymission-overview-state","","j"));
        const auto layoutBeforeReadback=nativeLayoutState();
        const auto readbackStart=std::chrono::steady_clock::now();
        std::vector<uint8_t> rgba(frame.width*frame.height*4);
        if (!readFramePixels(*fb,rgba)) throw std::runtime_error("private framebuffer readback failed");
        frame.pixels.resize(frame.width*frame.height*3);
        for (size_t i=0;i<frame.pixels.size()/3;++i) std::copy_n(rgba.data()+i*4,3,frame.pixels.data()+i*3);
        auto tiles=nlohmann::json::parse(HyprlandAPI::invokeHyprctlCommand("hymission-tiles-state","","j"),nullptr,false);
        if (tiles.is_discarded()) tiles={{"enabled",false},{"tiles",nlohmann::json::array()}};
        frame.state={{"overview",overviewBeforeReadback}, {"tiles",tiles}};
        frame.state["layout"]=layoutBeforeReadback;
        frame.state["monitor"]=ctx.m_data.pMonitor->m_name;
        frame.state["framebuffer"]={{"drm_format",fb->getTexture()->m_drmFormat},{"format",fb->m_drmFormat},{"mirror_format",fb->getMirrorTexture()?fb->getMirrorTexture()->m_drmFormat:0},{"current",fb->getFBID()},{"main",dynamicPointerCast<Render::GL::CGLFramebuffer>(ctx.m_data.mainFB)->getFBID()},{"damage",nlohmann::json::array()}};
        ctx.m_data.damage.forEachRect([&](const auto& rect) { frame.state["framebuffer"]["damage"].push_back({rect.x1,rect.y1,rect.x2,rect.y2}); });
        frame.state["stage"]=stageBeforeReadback;
        const auto focused=Desktop::focusState()->window();
        frame.state["focus"]=focused?std::format("0x{:x}",reinterpret_cast<uintptr_t>(focused.get())):"";
        frame.state["readback_ms"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-readbackStart).count();
        frame.state["native"]=nlohmann::json::array();
        for (const auto& window:Desktop::windowState()->windows()) {
            if (!window->mapped() || !window->m_workspace) continue;
            const auto ws=window->m_workspace;const auto offset=ws->m_renderOffset->value();
            frame.state["native"].push_back({{"address",std::format("0x{:x}",reinterpret_cast<uintptr_t>(window.get()))},
                {"size",{window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x,window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y}},{"workspace",ws->displayName()},{"offset",{offset.x,offset.y}},
                {"floating_offset",{window->presentation().floatingOffset().x,window->presentation().floatingOffset().y}},
                {"workspace_alpha",ws->m_alpha->value()},{"workspace_animated",ws->m_renderOffset->isBeingAnimated()},{"workspace_alpha_animated",ws->m_alpha->isBeingAnimated()},{"visible",ws->visible()},{"forced",ws->m_forceRendering},
                {"alpha",window->presentation().alphaTotal()},
                {"active_alpha",window->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE)},
                {"dim",window->presentation().dimPercent()},
                {"active_workspace",ws->m_monitor && ws->m_monitor->m_activeWorkspace==ws},
                {"offset_goal",{ws->m_renderOffset->goal().x,ws->m_renderOffset->goal().y}},
                {"presentation_alpha",window->presentation().renderPresentation().alpha},
                {"presentation_fade",window->presentation().renderPresentation().fadeAlpha},{"fullscreen_alpha",window->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN)},
                {"position",{window->positionAnimation()->value().x,window->positionAnimation()->value().y}}});
        }
        capturedFrames.push_back(std::move(frame));return {};
    }
    bool needsLiveBlur(Render::CRenderContext&) override { return false; }
    bool needsPrecomputeBlur(Render::CRenderContext&) override { return false; }
    bool undiscardable(Render::CRenderContext&) override { return true; }
    const char* passName() override { return "private-frame-capture"; }
    ePassElementType type() override { return EK_CUSTOM; }
};
int frameCapture(lua_State* L) {
    const std::string action=luaL_checkstring(L,1);
    if (action=="begin") { capturedFrames.clear();captureMonitor=luaL_optstring(L,2,"");recordingFrames=true; }
    else if (action=="end") {
        recordingFrames=false;const std::string prefix=luaL_checkstring(L,2);
        nlohmann::json states=nlohmann::json::array();
        for (size_t i=0;i<capturedFrames.size();++i) {
            const auto& frame=capturedFrames[i];const std::string path=prefix+"-"+std::to_string(i)+".ppm";
            std::ofstream out(path,std::ios::binary);out<<"P6\n"<<frame.width<<" "<<frame.height<<"\n255\n";
            // Native workbuffer projection stores logical top-to-bottom rows.
            for (int y=0;y<frame.height;++y) out.write(reinterpret_cast<const char*>(frame.pixels.data()+y*frame.width*3),frame.width*3);
            if (!out) return luaL_error(L,"private frame write failed");
            auto state=frame.state;state["pixels"]=path;states.push_back(std::move(state));
        }
        std::ofstream out(prefix+".json");out<<states.dump(2)<<"\n";capturedFrames.clear();
    } else return luaL_error(L,"unknown private frame capture action");
    return 0;
}
using Clock=std::chrono::steady_clock;
struct GLCounters { uint64_t uniform_lookups,integer_reads,texture_allocations,texture_pixels,framebuffer_allocations,clears,clear_pixels,draws,shader_links; };
struct FrameProfile {
    void (*resetGL)()=nullptr;
    void (*snapshotGL)(GLCounters*)=nullptr;
    bool active=false, gpuAvailable=false;
    GLuint query=0;
    Clock::time_point start{},previous{};
    double interval=0,layoutMs=0,layoutOutsideMs=0;
    size_t layoutCalls=0,symbolCalls=0;
    std::string gpu,reason;
    uint32_t drmFormat=0,mirrorFormat=0;
    nlohmann::json frames=nlohmann::json::array();
    std::vector<std::pair<GLuint,size_t>> pending;
    PFNGLGETQUERYOBJECTUI64VEXTPROC result=nullptr;
    void drain(bool finish=false) {
        if (eglGetCurrentContext()==EGL_NO_CONTEXT) return;
        if (finish) glFinish();
        std::erase_if(pending,[&](const auto& p) {
            GLuint available=0; glGetQueryObjectuiv(p.first,GL_QUERY_RESULT_AVAILABLE,&available);
            if (!available) return false;
            GLuint64 value=0; result(p.first,GL_QUERY_RESULT_EXT,&value);
            frames[p.second]["gpu_ms"]=value/1e6;
            glDeleteQueries(1,&p.first); return true;
        });
    }
    void stage(Event::SRenderStageEvent event) {
        if (!active) return;
        if (event.stage==RENDER_PRE) {
            drain();
            if (resetGL) resetGL();
            const auto now=Clock::now();
            interval=previous==Clock::time_point{}?0:std::chrono::duration<double,std::milli>(now-previous).count();
            previous=start=now;
        } else if (event.stage==RENDER_BEGIN && event.context && !query) {
            const auto fb=event.context->get().m_data.currentFB;
            drmFormat=fb?fb->m_drmFormat:0;
            const auto mirror=fb?fb->getMirrorTexture():nullptr;
            mirrorFormat=mirror?mirror->m_drmFormat:0;
            if (!result) {
                const auto ext=reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
                result=reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
                gpuAvailable=ext && std::strstr(ext,"GL_EXT_disjoint_timer_query") && result;
                gpu=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
                if (!gpuAvailable) reason="GL_EXT_disjoint_timer_query unavailable";
            }
            if (gpuAvailable) { glGenQueries(1,&query); glBeginQuery(GL_TIME_ELAPSED_EXT,query); }
        } else if (event.stage==RENDER_POST && start!=Clock::time_point{}) {
            const auto cpu=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
            frames.push_back({{"cpu_work_ms",cpu+layoutOutsideMs},{"layout_ms",layoutMs},{"layout_calls",layoutCalls},{"symbol_calls",symbolCalls},{"cpu_render_ms",cpu},{"interval_ms",interval},{"time_ns",std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count()}});
            if (snapshotGL) {
                GLCounters c{};snapshotGL(&c);
                frames.back()["gl"]={{"uniform_lookups",c.uniform_lookups},{"integer_reads",c.integer_reads},{"texture_allocations",c.texture_allocations},{"texture_pixels",c.texture_pixels},{"framebuffer_allocations",c.framebuffer_allocations},{"clears",c.clears},{"clear_pixels",c.clear_pixels},{"draws",c.draws},{"shader_links",c.shader_links}};
            }
            if (query) { glEndQuery(GL_TIME_ELAPSED_EXT); pending.emplace_back(query,frames.size()-1); query=0; }
            start={};layoutMs=layoutOutsideMs=0;layoutCalls=symbolCalls=0;
        }
    }
} frameProfile;
Hyprutils::Memory::CSharedPointer<Hyprutils::Signal::CSignalListener> frameListener;
extern HANDLE fixtureHandle;
CFunctionHook *profileLayoutHook=nullptr,*profileSymbolsHook=nullptr;
void profileLayout(Layout::CLayoutManager* manager,PHLMONITOR monitor) {
    const auto begin=Clock::now();
    reinterpret_cast<decltype(&profileLayout)>(profileLayoutHook->m_original)(manager,monitor);
    if (frameProfile.active) {
        const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        frameProfile.layoutMs+=elapsed;++frameProfile.layoutCalls;
        if (frameProfile.start==Clock::time_point{}) frameProfile.layoutOutsideMs+=elapsed;
    }
}
std::vector<SFunctionMatch> profileSymbols(HANDLE handle,const std::string& name) {
    if (frameProfile.active) ++frameProfile.symbolCalls;
    return reinterpret_cast<decltype(&profileSymbols)>(profileSymbolsHook->m_original)(handle,name);
}
int profile(lua_State* L) {
    const std::string action=luaL_checkstring(L,1);
    if (action=="begin") {
        if (!profileLayoutHook) {
            const auto source=hymission::hooks::findSymbol(HyprlandAPI::findFunctionsByName(fixtureHandle,"invalidateMonitorGeometries"),"Layout::CLayoutManager::invalidateMonitorGeometries(Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>)",true);
            profileLayoutHook=HyprlandAPI::createFunctionHook(fixtureHandle,source,reinterpret_cast<void*>(&profileLayout));
            profileSymbolsHook=HyprlandAPI::createFunctionHook(fixtureHandle,reinterpret_cast<void*>(&HyprlandAPI::findFunctionsByName),reinterpret_cast<void*>(&profileSymbols));
            if (!profileLayoutHook || !profileLayoutHook->hook() || !profileSymbolsHook || !profileSymbolsHook->hook()) return luaL_error(L,"private profiler hooks unavailable");
        }
        frameProfile.resetGL=reinterpret_cast<void(*)()>(dlsym(RTLD_DEFAULT,"stage_profile_gl_reset"));
        frameProfile.snapshotGL=reinterpret_cast<void(*)(GLCounters*)>(dlsym(RTLD_DEFAULT,"stage_profile_gl_snapshot"));
        frameProfile.drain(true);frameProfile.layoutMs=frameProfile.layoutOutsideMs=0;frameProfile.layoutCalls=frameProfile.symbolCalls=0;
        frameProfile.frames=nlohmann::json::array();frameProfile.previous={};frameProfile.active=true;
    } else if (action=="end") {
        frameProfile.active=false;
        frameProfile.drain(true);
        std::ofstream output(luaL_checkstring(L,2));
        output<<nlohmann::json{{"drm_format",frameProfile.drmFormat},{"mirror_format",frameProfile.mirrorFormat},{"gpu_available",frameProfile.gpuAvailable},{"gpu",frameProfile.gpu},{"gpu_error",frameProfile.reason},{"pending",frameProfile.pending.size()},{"frames",frameProfile.frames}}.dump(2)<<"\n";
        if (!output) return luaL_error(L,"cannot write private frame profile");
    } else return luaL_error(L,"unknown profile action");
    return 0;
}
HANDLE fixtureHandle=nullptr;
CFunctionHook* releaseHook=nullptr;
int mouseReleases=0;
void releaseButtons(CInputManager* input) {
    ++mouseReleases;
    reinterpret_cast<decltype(&releaseButtons)>(releaseHook->m_original)(input);
}
int workspaceChange(lua_State* L) {
    if (!releaseHook) {
        const auto source=hymission::hooks::findSymbol(HyprlandAPI::findFunctionsByName(fixtureHandle,"releaseAllMouseButtons"),"CInputManager::releaseAllMouseButtons()",true);
        if (source) releaseHook=HyprlandAPI::createFunctionHook(fixtureHandle,source,reinterpret_cast<void*>(&releaseButtons));
        if (!releaseHook || !releaseHook->hook()) return luaL_error(L,"test release spy unavailable");
    }
    const auto mon=Desktop::focusState()->monitor();
    const auto target=State::Workspace::resolver()->getWorkspaceTargetFromString(luaL_checkstring(L,1),mon);
    auto ws=State::workspaceState()->find(target);
    if (!ws) ws=State::workspaceState()->create(target,mon,true);
    const int before=mouseReleases;
    mon->changeWorkspace(ws,lua_toboolean(L,2),true);
    lua_pushinteger(L,mouseReleases-before);
    return 1;
}
int symbolReport(lua_State* L) {
    const std::string query=luaL_checkstring(L,1);
    nlohmann::json report=nlohmann::json::array();
    for (const auto& symbol:HyprlandAPI::findFunctionsByName(fixtureHandle,query))
        report.push_back({{"signature",symbol.signature},{"api_demangled",symbol.demangled},{"canonical",hymission::hooks::canonicalSymbol(symbol.signature)},{"address",std::format("0x{:x}",reinterpret_cast<uintptr_t>(symbol.address))}});
    std::ofstream output(luaL_checkstring(L,2));
    output<<report.dump(2)<<"\n";
    if (!output) return luaL_error(L,"cannot write private symbol report");
    return 0;
}
int meta(lua_State* L) {
    const auto keyboard=g_pSeatManager->m_keyboard.lock();
    if (!keyboard || !keyboard->m_xkbKeymap || !keyboard->m_xkbState) return luaL_error(L,"isolated keyboard unavailable");
    const auto index=xkb_keymap_mod_get_index(keyboard->m_xkbKeymap,XKB_MOD_NAME_LOGO);
    if (index==XKB_MOD_INVALID) return luaL_error(L,"isolated Meta modifier unavailable");
    const auto mask=lua_toboolean(L,1)?1u<<index:0;
    keyboard->m_modifiersState.depressed=mask;
    xkb_state_update_mask(keyboard->m_xkbState,mask,0,0,0,0,0);
    g_pInputManager->onKeyboardMod(keyboard);
    return 0;
}
int animationReport(lua_State* L) {
    nlohmann::json report=nlohmann::json::object();
    for (const auto& ws:State::workspaceState()->workspacesCopy()) {
        const auto& offset=ws->m_renderOffset;
        const auto cfg=offset->getConfig().lock();
        const auto values=cfg?cfg->pValues.lock():nullptr;
        report[ws->addressableName()]={{"value",{offset->value().x,offset->value().y}},{"goal",{offset->goal().x,offset->goal().y}},
            {"begun",{offset->begun().x,offset->begun().y}},{"alpha",ws->m_alpha->value()},{"alpha_goal",ws->m_alpha->goal()},
            {"percent",offset->getPercent()},{"curve_value",offset->getCurveValue()},{"curve",offset->getBezierName()},
            {"speed",values?values->internalSpeed:0},{"style",offset->getStyle()},
            {"animated",offset->isBeingAnimated() || ws->m_alpha->isBeingAnimated()}};
    }
    std::ofstream output(luaL_checkstring(L,1)); output<<report.dump(2)<<"\n";
    if (!output) return luaL_error(L,"cannot write private animation report");
    return 0;
}
int workspaceAnimation(lua_State* L) {
    const auto target=State::Workspace::resolver()->getWorkspaceTargetFromString(luaL_checkstring(L,1),Desktop::focusState()->monitor());
    const auto ws=State::workspaceState()->find(target);
    if (!ws) return luaL_error(L,"test workspace not found");
    lua_createtable(L,0,5);
    lua_pushnumber(L,ws->m_renderOffset->value().x); lua_setfield(L,-2,"x");
    lua_pushnumber(L,ws->m_renderOffset->goal().x); lua_setfield(L,-2,"goal_x");
    lua_pushnumber(L,ws->m_alpha->value()); lua_setfield(L,-2,"alpha");
    lua_pushboolean(L,ws->m_renderOffset->isBeingAnimated() || ws->m_alpha->isBeingAnimated()); lua_setfield(L,-2,"animated");
    return 1;
}
int refresh(lua_State*) { Event::bus()->m_events.config.reloaded.emit(); return 0; }
PHLWORKSPACE specialOrigin;
PHLWINDOW specialWindow;
int special(lua_State* L) {
    auto mon=Desktop::focusState()->monitor();
    if (lua_toboolean(L,1)) {
        specialWindow=Desktop::focusState()->window();
        if (!specialWindow) return luaL_error(L,"no focused test window");
        specialOrigin=specialWindow->m_workspace;
        const auto target=State::Workspace::resolver()->getWorkspaceTargetFromString("special:tiles-test",mon);
        auto ws=State::workspaceState()->find(target);
        if (!ws) ws=State::workspaceState()->create(target,mon,true);
        Desktop::globalWindowController()->moveWindowToWorkspace(specialWindow,ws);
        mon->setSpecialWorkspace(ws);
    } else {
        mon->setSpecialWorkspace(nullptr);
        if (specialWindow && specialOrigin) Desktop::globalWindowController()->moveWindowToWorkspace(specialWindow,specialOrigin);
        specialWindow.reset(); specialOrigin.reset();
    }
    return 0;
}
int window(lua_State* L) {
    const std::string action=luaL_checkstring(L,1);
    const std::string address=luaL_optstring(L,2,"");
    PHLWINDOW window;
    for (const auto& w:Desktop::windowState()->windows()) if (std::format("0x{:x}",reinterpret_cast<uintptr_t>(w.get()))==address) window=w;
    if (!window) return luaL_error(L,"test window not found");
    if (action=="workspace") {
        const auto destination=hymission::workspaceForKey(luaL_checkinteger(L,3));
        if (!destination) return luaL_error(L,"test workspace not found");
        Desktop::globalWindowController()->moveWindowToWorkspace(window,destination);
    } else if (action=="place") {
        const CBox geometry{luaL_checknumber(L,3),luaL_checknumber(L,4),luaL_checknumber(L,5),luaL_checknumber(L,6)};
        if (window->isFloating()) window->layoutTarget()->space()->algorithm()->setTargetGeom(geometry,window->layoutTarget());
        else window->layoutTarget()->setPositionGlobal(geometry);
        window->positionAnimation()->warp(); window->sizeAnimation()->warp();
        g_pHyprRenderer->damageMonitor(window->m_monitor.lock());
    } else if (action=="assert-hit") {
        const Vector2D point{luaL_checknumber(L,3),luaL_checknumber(L,4)};
        const auto hit=Desktop::viewState()->hitTest().windowAt(point,Desktop::View::ALLOW_FLOATING | Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS);
        if (hit!=window) return luaL_error(L,"pointer did not hit expected window");
    } else if (action=="layout-fullscreen") {
        Fullscreen::controller()->setFullscreenMode(window,Fullscreen::FSMODE_FULLSCREEN,Fullscreen::FSMODE_FULLSCREEN,true);
    } else if (action=="internal-fullscreen" || action=="layout-internal-fullscreen") {
        const auto result=Config::Actions::fullscreenWindow(Fullscreen::FSMODE_FULLSCREEN,Fullscreen::FSMODE_NONE,action=="layout-internal-fullscreen",window);
        if (!result) return luaL_error(L,"native internal fullscreen action failed");
    } else if (action=="focus") Desktop::focusState()->fullWindowFocus(window,Desktop::FOCUS_REASON_KEYBIND);
    else if (action=="reset_width") {
        Desktop::focusState()->fullWindowFocus(window,Desktop::FOCUS_REASON_KEYBIND);
        const auto algorithm=window->layoutTarget()->space()->algorithm();
        if (!algorithm || !algorithm->tiledAlgo() || !algorithm->tiledAlgo()->layoutMsg("colresize all 0.32"))
            return luaL_error(L,"native scrolling width reset failed");
    }
    else if (action=="fullscreen" || action=="maximize" || action=="normal") {
        const auto mode=action=="fullscreen"?Fullscreen::FSMODE_FULLSCREEN:action=="maximize"?Fullscreen::FSMODE_MAXIMIZED:Fullscreen::FSMODE_NONE;
        Fullscreen::controller()->setFullscreenMode(window,mode,mode,false);
    } else if (action=="float" || action=="tile") {
        auto result=Config::Actions::floatWindow(action=="float"?Config::Actions::TOGGLE_ACTION_ENABLE:Config::Actions::TOGGLE_ACTION_DISABLE,window);
        if (!result) return luaL_error(L,"native float operation failed");
    } else if (action=="pin" || action=="unpin") {
        auto result=Config::Actions::floatWindow(Config::Actions::TOGGLE_ACTION_ENABLE,window);
        if (result) result=Config::Actions::pinWindow(action=="pin"?Config::Actions::TOGGLE_ACTION_ENABLE:Config::Actions::TOGGLE_ACTION_DISABLE,window);
        if (!result) return luaL_error(L,"native pin operation failed");
    } else return luaL_error(L,"unknown test window action");
    return 0;
}
// Private native drag controller entry, the same owner used by mouse binds.
int drag(lua_State* L) {
    const std::string action=luaL_checkstring(L,1);
    auto* drag=g_layoutManager->dragController().get();
    if (action=="begin") {
        const std::string address=luaL_checkstring(L,2);
        PHLWINDOW window;
        for (const auto& w:Desktop::windowState()->windows())
            if (std::format("0x{:x}",reinterpret_cast<uintptr_t>(w.get()))==address) window=w;
        if (!window) return luaL_error(L,"drag test window not found");
        const Vector2D point{luaL_checknumber(L,3),luaL_checknumber(L,4)};
        Pointer::pointerController()->warpTo(point);
        drag->dragBegin(window->layoutTarget(),MBIND_MOVE);
        if (!drag->target()) return luaL_error(L,"native drag did not begin");
    } else if (action=="move") {
        const Vector2D point{luaL_checknumber(L,2),luaL_checknumber(L,3)};
        Pointer::pointerController()->warpTo(point);
        drag->mouseMove(point);
    } else if (action=="end") drag->dragEnd();
    else return luaL_error(L,"unknown native drag action");
    return 0;
}
// Emit device-level swipe events on the private nested compositor pointer.
// PointerManager -> InputManager -> EventBus -> matching -> trackpad wrapper;
// it deliberately does not call the unified swipe or any dispatcher directly.
int resolveWorkspace(lua_State* L) {
    const auto target=State::Workspace::resolver()->getWorkspaceTargetFromString(luaL_checkstring(L,1),Desktop::focusState()->monitor());
    const auto key=target.valid()?hymission::workspaceKey(target):hymission::WORKSPACE_INVALID;
    if (lua_gettop(L)>1) { std::ofstream output(luaL_checkstring(L,2)); output<<key<<"\n"; }
    lua_pushinteger(L,key);
    return 1;
}
int key(lua_State* L) {
    const auto keyboard=g_pSeatManager->m_keyboard.lock();
    if (!keyboard) return luaL_error(L,"private keyboard unavailable");
    static uint32_t time=100;
    const uint32_t code=luaL_checkinteger(L,1);
    keyboard->m_keyboardEvents.key.emit({.timeMs=time++,.keycode=code,.state=WL_KEYBOARD_KEY_STATE_PRESSED});
    keyboard->m_keyboardEvents.key.emit({.timeMs=time++,.keycode=code,.state=WL_KEYBOARD_KEY_STATE_RELEASED});
    return 0;
}
int trackpad(lua_State* L) {
    const std::string action=luaL_checkstring(L,1);
    static uint32_t time=1;
    const auto pointer=g_pInputManager->m_pointers.empty()?nullptr:g_pInputManager->m_pointers.front();
    if (!pointer) return luaL_error(L,"private compositor pointer unavailable");
    if (action=="begin") pointer->m_pointerEvents.swipeBegin.emit({.timeMs=time++,.fingers=4});
    else if (action=="update") pointer->m_pointerEvents.swipeUpdate.emit({.timeMs=time++,.fingers=4,.delta={luaL_optnumber(L,3,0.0),luaL_checknumber(L,2)}});
    else if (action=="end" || action=="cancel") pointer->m_pointerEvents.swipeEnd.emit({.timeMs=time++,.cancelled=action=="cancel"});
    else return luaL_error(L,"unknown trackpad event");
    return 0;
}
int swipe(lua_State* L) {
    const std::string action=luaL_checkstring(L,1);
    if (action=="begin" || action=="try_begin") {
        const bool started=g_pUnifiedWorkspaceSwipe->begin(Desktop::focusState()->monitor());
        const bool active=started && g_pUnifiedWorkspaceSwipe->isGestureInProgress();
        if (!active && action=="begin") return luaL_error(L,"native swipe did not begin");
        lua_pushboolean(L,active); return 1;
    }
    if (action=="update") g_pUnifiedWorkspaceSwipe->update(luaL_checknumber(L,2));
    else if (action=="end") g_pUnifiedWorkspaceSwipe->end();
    else if (action=="cancel") g_pUnifiedWorkspaceSwipe->cancel();
    else return luaL_error(L,"unknown test swipe action");
    return 0;
}
// Controlled focus-dependent native layout resize, confined to private tests.
CHyprSignalListener focusResizeListener;
int focusResize(lua_State* L) {
    focusResizeListener.reset();
    if (lua_toboolean(L,1)) focusResizeListener=Event::bus()->m_events.window.active.listen([](PHLWINDOW w,Desktop::eFocusReason) {
        if (!w || !w->layoutTarget() || !w->layoutTarget()->space()) return;
        const auto algorithm=w->layoutTarget()->space()->algorithm();
        const auto width=w->size(Desktop::View::IGeometric::GEOMETRIC_GOAL).x;
        const auto area=w->layoutTarget()->space()->workArea();
        if (algorithm && algorithm->tiledAlgo()) (void)algorithm->tiledAlgo()->layoutMsg(width < area.w*.5 ? "colresize 0.65" : "colresize 0.35");
    });
    return 0;
}
int pointer(lua_State* L) {
    const std::string action=luaL_checkstring(L,1);
    if (action.starts_with("device_")) {
        const auto device=g_pInputManager->m_pointers.empty()?nullptr:g_pInputManager->m_pointers.front();
        if (!device) return luaL_error(L,"private pointer unavailable");
        static uint32_t time=1000;
        const Vector2D pos{luaL_checknumber(L,2),luaL_checknumber(L,3)};
        if (action=="device_move") {
            const auto delta=pos-g_pInputManager->getMouseCoordsInternal();
            device->m_pointerEvents.motion.emit({.timeMs=time++,.delta=delta,.unaccel=delta,.mouse=true,.device=device});
        } else device->m_pointerEvents.button.emit({.timeMs=time++,.button=BTN_LEFT,
            .state=action=="device_press"?WL_POINTER_BUTTON_STATE_PRESSED:WL_POINTER_BUTTON_STATE_RELEASED,.mouse=true});
        lua_pushboolean(L,g_layoutManager->dragController()->target()!=nullptr); return 1;
    }
    Event::SCallbackInfo info;
    if (action=="escape" || action=="tab" || action=="right" || action=="left") {
        const uint32_t key=action=="escape"?KEY_ESC:action=="tab"?KEY_TAB:action=="right"?KEY_RIGHT:KEY_LEFT;
        IKeyboard::SKeyEvent e{.keycode=key,.state=WL_KEYBOARD_KEY_STATE_PRESSED};
        Event::bus()->m_events.input.keyboard.key.emit(e,info);
    } else {
        const Vector2D pos{luaL_checknumber(L,2),luaL_checknumber(L,3)};
        Pointer::pointerController()->warpTo(pos);
        if (action=="native") g_pInputManager->refocus(pos);
        else if (action=="move") Event::bus()->m_events.input.mouse.move.emit(pos,info);
        else {
            IPointer::SButtonEvent e{.button=BTN_LEFT,.state=action=="press"?WL_POINTER_BUTTON_STATE_PRESSED:WL_POINTER_BUTTON_STATE_RELEASED};
            Event::bus()->m_events.input.mouse.button.emit(e,info);
        }
    }
    lua_pushboolean(L,info.cancelled); return 1;
}
}
APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    fixtureHandle=handle;
    captureDamageListener=Event::bus()->m_events.render.preChecks.listen([](PHLMONITOR monitor) {
        if (recordingFrames && (captureMonitor.empty() || captureMonitor==monitor->m_name)) g_pHyprRenderer->damageMonitor(monitor);
    });
    frameListener=Event::bus()->m_events.render.stage.listen([](Event::SRenderStageEvent event) { frameProfile.stage(event);
        if (recordingFrames && event.stage==RENDER_LAST_MOMENT && event.context) event.context->get().m_pass.add(makeUnique<CaptureFramePass>()); });
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","profile",profile);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","frame_capture",frameCapture);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","layout_state",layoutState);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","focus_resize",focusResize);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","trackpad",trackpad);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","key",key);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","resolve_workspace",resolveWorkspace);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","drag",drag);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","animation_report",animationReport);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","symbol_report",symbolReport);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","change_workspace",workspaceChange);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","meta",meta);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","workspace_animation",workspaceAnimation);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","refresh",refresh);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","window",window);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","special",special);
    if (!HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","swipe",swipe) ||
        !HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","pointer",pointer)) throw std::runtime_error("test fixture Lua registration failed");
    return {"hymission_tiles_test","private native gesture/input test fixture","hymission","1"};
}
APICALL EXPORT void PLUGIN_EXIT() {
    recordingFrames=false;capturedFrames.clear();frameProfile.active=false;captureDamageListener.reset();frameListener.reset();
    focusResizeListener.reset();
    if (profileLayoutHook) HyprlandAPI::removeFunctionHook(fixtureHandle,profileLayoutHook);
    if (profileSymbolsHook) HyprlandAPI::removeFunctionHook(fixtureHandle,profileSymbolsHook);
    g_pUnifiedWorkspaceSwipe->cancel();
    if (releaseHook) HyprlandAPI::removeFunctionHook(fixtureHandle,releaseHook);
    releaseHook=nullptr;
}
