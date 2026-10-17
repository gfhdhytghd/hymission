#pragma once
#include "tiles_logic.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
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
    explicit TilesController(HANDLE, std::function<bool()> overviewVisible);
    ~TilesController();
    void initialize();
    SDispatchResult dispatch(const std::string& action, const std::string& args);
    std::string stateJson() const;
    static TilesController* get();
    std::vector<View> views(const PHLMONITOR&) const;
    std::optional<View> view(const PHLWORKSPACE&) const;
    bool enabled(const PHLMONITOR&) const;
    bool presented(const PHLWORKSPACE&) const;
    bool focus(const PHLMONITOR&, tiles::Id, bool warp = false);
    bool activate(const PHLMONITOR&, tiles::Id, const PHLWORKSPACE&, bool swap = false);
    bool moveWindow(const PHLWINDOW&, const PHLMONITOR&, tiles::Id);
    bool busy() const;
    enum class Owner { None, Resize, Swipe, OverviewDrag, StageDrag };
    bool acquire(Owner);
    void release(Owner);
    Owner owner() const;
    // Purely visual shared edge service diagnostics.
    void edgeError(std::string error);
  private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}
