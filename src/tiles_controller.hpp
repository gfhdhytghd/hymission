#pragma once
#include "tiles_logic.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hymission {
class TilesController {
  public:
    struct View {
        PHLMONITOR monitor;
        PHLWORKSPACE workspace;
        tiles::Id id = 0;
        CBox allocation;
        bool focused = false;
    };
    explicit TilesController(HANDLE, std::function<bool()> overviewVisible, std::function<void()> topologyChanged = {}, std::function<bool()> overviewGrouped = {});
    ~TilesController();
    void initialize();
    SDispatchResult dispatch(const std::string& action, const std::string& args);
    std::string stateJson() const;
    static TilesController* get();
    std::vector<View> views(const PHLMONITOR&) const;
    std::optional<View> view(const PHLWORKSPACE&) const;
    bool enabled(const PHLMONITOR&) const;
    bool available() const;
    void fail(std::string error);
    bool handleDivider(const IPointer::SButtonEvent&, Event::SCallbackInfo&);
    tiles::Id preferredTile(const PHLWORKSPACE&, const PHLMONITOR&) const;
    bool presented(const PHLWORKSPACE&) const;
    bool focus(const PHLMONITOR&, tiles::Id, bool warp = false);
    bool activate(const PHLMONITOR&, tiles::Id, const PHLWORKSPACE&, bool swap = false);
    bool moveWorkspace(const PHLWORKSPACE&, const PHLMONITOR&, tiles::Id);
    bool moveWindow(const PHLWINDOW&, const PHLMONITOR&, tiles::Id);
    bool moveWindows(const std::vector<PHLWINDOW>&, const PHLMONITOR&, tiles::Id, const PHLWORKSPACE&);
    bool busy() const;
    enum class Owner { None, Resize, Swipe, OverviewDrag, StageDrag };
    bool acquire(Owner);
    void release(Owner);
    Owner owner() const;
    // Purely visual shared edge service diagnostics.
    void edgeError(std::string error);
    void edgeActive();
    bool beginTransfer(const std::vector<PHLWINDOW>&, const PHLMONITOR&, tiles::Id, const CBox&, const Vector2D&);
    bool transferDestination(const PHLMONITOR& source, tiles::Id, const Vector2D&, const PHLMONITOR& columnMonitor, tiles::Id column) const;
    bool updateTransfer(const Vector2D&, const PHLMONITOR& columnMonitor = {}, tiles::Id column = 0, const PHLWORKSPACE& target = {});
    bool finishTransfer(bool commit);
    bool transferDragging() const;
    bool transferMember(const PHLWINDOW&) const;
    std::vector<PHLWINDOW> transferMembers() const;
    Vector2D transferPoint() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}
