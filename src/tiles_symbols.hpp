#pragma once
#include <string_view>
// Exact development signatures, audited against e5a03b6a. No prefix matching.
namespace hymission::tiles::symbols {
inline std::string_view lookup(std::string_view key) {
    if (key == "recheckWorkArea") return "Layout::CSpace::recheckWorkArea()";
    if (key == "windowAt") return "Desktop::CViewHitTester::windowAt(Hyprutils::Math::Vector2D const&, unsigned short, Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>) const";
    if (key == "rawWindowFocus") return "Desktop::CFocusState::rawWindowFocus(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Desktop::eFocusReason, Hyprutils::Memory::CSharedPointer<CWLSurfaceResource>)";
    if (key == "changeWorkspaceMonitor") return "Monitor::CMonitor::changeWorkspace(Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace> const&, bool, bool, bool)";
    if (key == "isVisibleNotCovered") return "Workspace::CHLWorkspace::isVisibleNotCovered() const";
    if (key == "renderWindow") return "Render::IHyprRenderer::renderWindow(Render::CRenderContext&, Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, Render::SWindowRenderPresentation const&, std::chrono::time_point<std::chrono::_V2::steady_clock, std::chrono::duration<long, std::ratio<1l, 1000000000l> > > const&, bool, Render::eRenderPassMode, bool, bool)";
    if (key == "renderWorkspaceWindows") return "Render::IHyprRenderer::renderWorkspaceWindows(Render::CRenderContext&, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>, std::chrono::time_point<std::chrono::_V2::steady_clock, std::chrono::duration<long, std::ratio<1l, 1000000000l> > > const&, Render::eSceneMode)";
    if (key == "renderWorkspaceWindowsFullscreen") return "Render::IHyprRenderer::renderWorkspaceWindowsFullscreen(Render::CRenderContext&, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>, std::chrono::time_point<std::chrono::_V2::steady_clock, std::chrono::duration<long, std::ratio<1l, 1000000000l> > > const&, Render::eSceneMode)";
    if (key == "sendFrameEventsToWorkspace") return "Render::IHyprRenderer::sendFrameEventsToWorkspace(Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>, std::chrono::time_point<std::chrono::_V2::steady_clock, std::chrono::duration<long, std::ratio<1l, 1000000000l> > > const&)";
    if (key == "setTargetSizeAndPosition") return "Fullscreen::IFullscreenHandler::setTargetSizeAndPosition(Hyprutils::Memory::CSharedPointer<Layout::ITarget>)";
    if (key == "syncTargetSizeAndPosition") return "Fullscreen::IFullscreenHandler::syncTargetSizeAndPosition()";
    if (key == "changeWorkspaceTyped") return "Config::Actions::changeWorkspace(Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>)";
    if (key == "changeWorkspaceCurrentTyped") return "Config::Actions::changeWorkspaceOnCurrentMonitor(Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>)";
    if (key == "changeWorkspaceString") return "Config::Actions::changeWorkspace(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&)";
    if (key == "changeWorkspaceCurrentString") return "Config::Actions::changeWorkspaceOnCurrentMonitor(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&)";
    if (key == "getWorkspaceTargetFromString") return "State::Workspace::CWorkspaceResolver::getWorkspaceTargetFromString(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, std::optional<Hyprutils::Memory::CSharedPointer<Monitor::CMonitor> >)";
    if (key == "previousWorkspace") return "Desktop::History::CWorkspaceHistoryTracker::previousWorkspace(Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>)";
    if (key == "previousWorkspaceMonitor") return "Desktop::History::CWorkspaceHistoryTracker::previousWorkspace(Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>)";
    if (key == "moveFocus") return "Config::Actions::moveFocus(Math::eDirection)";
    if (key == "moveInDirection") return "Config::Actions::moveInDirection(Math::eDirection, std::optional<Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow> >)";
    if (key == "moveWorkspaceToMonitor") return "State::Workspace::CPlacementController::moveWorkspaceToMonitor(Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, bool, bool) const";
    if (key == "swapActiveWorkspaces") return "State::Workspace::CPlacementController::swapActiveWorkspaces(Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>) const";
    if (key == "begin") return "CUnifiedWorkspaceSwipeGesture::begin(Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>)";
    if (key == "update") return "CUnifiedWorkspaceSwipeGesture::update(double)";
    if (key == "end") return "CUnifiedWorkspaceSwipeGesture::end()";
    if (key == "cancel") return "CUnifiedWorkspaceSwipeGesture::cancel()";
    if (key == "mouseMoveUnified") return "CInputManager::mouseMoveUnified(unsigned int, bool, bool, std::optional<Hyprutils::Math::Vector2D>, bool)";
    if (key == "hasFullscreenWorkspace") return "Fullscreen::CFullscreenController::hasFullscreen(Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>, std::optional<bool>)";
    if (key == "fullscreenModesWorkspace") return "Fullscreen::CFullscreenController::getFullscreenModes(Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>, std::optional<bool>)";
    if (key == "fullscreenWindowWorkspace") return "Fullscreen::CFullscreenController::getFullscreenWindow(Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>, std::optional<bool>)";
    if (key == "setFullscreenFadeAnimation") return "Animation::Workspace::setFullscreenFadeAnimation(Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>, Animation::Workspace::eAnimationType)";
    if (key == "renderLayer") return "Render::IHyprRenderer::renderLayer(Render::CRenderContext&, Hyprutils::Memory::CSharedPointer<Desktop::View::CLayerSurface>, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, std::chrono::time_point<std::chrono::_V2::steady_clock, std::chrono::duration<long, std::ratio<1l, 1000000000l> > > const&, bool, bool)";
    if (key == "layerSurfaceAt") return "Desktop::CViewHitTester::layerSurfaceAt(Hyprutils::Math::Vector2D const&, std::vector<Hyprutils::Memory::CWeakPointer<Desktop::View::CLayerSurface>, std::allocator<Hyprutils::Memory::CWeakPointer<Desktop::View::CLayerSurface> > >*, Hyprutils::Math::Vector2D*, Hyprutils::Memory::CSharedPointer<Desktop::View::CLayerSurface>*, bool) const";
    if (key == "logicalBox") return "Monitor::CMonitor::logicalBox() const";
    if (key == "scrollRecalculate") return "Layout::Tiled::SScrollingData::recalculate(bool)";
    if (key == "scrollFocus") return "Layout::Tiled::CScrollingAlgorithm::focusOnInput(Hyprutils::Memory::CSharedPointer<Layout::ITarget>, Layout::Tiled::CScrollingAlgorithm::eInputMode)";
    if (key == "scrollFullscreenRequest") return "Fullscreen::ScrollingFullscreenHandler::CScrollingFullscreenHandler::requestFullscreen(Fullscreen::SFullscreenRequest const&)";
    if (key == "scrollFullscreenColumn") return "Fullscreen::ScrollingFullscreenHandler::CScrollingFullscreenHandler::fullscreenColumnWidth() const";
    return {};
}
}
