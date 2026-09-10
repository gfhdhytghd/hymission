#pragma once

#include <functional>
#include <memory>
#include <string>
#include <hyprland/src/plugins/PluginAPI.hpp>

namespace hymission {

// Owns the persistent desktop mode. Overview only supplies a suspension predicate;
// no overview layout, window transform or input capture is used by this controller.
class StageController {
  public:
    StageController(HANDLE handle, std::function<bool()> overviewSuspended);
    ~StageController();
    StageController(const StageController&) = delete;
    StageController& operator=(const StageController&) = delete;
    void initialize();
    std::string stateJson() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace hymission
