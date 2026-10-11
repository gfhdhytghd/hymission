#pragma once

#include <string>

namespace hymission {
inline std::string normalizeHymissionDispatcher(std::string dispatcher) {
    if (dispatcher == "stage_open" || dispatcher == "hymission.stage_open")
        return "hymission:stage_open";
    if (dispatcher == "stage_close" || dispatcher == "hymission.stage_close")
        return "hymission:stage_close";
    if (dispatcher == "stage_toggle" || dispatcher == "hymission.stage_toggle")
        return "hymission:stage_toggle";
    if (dispatcher == "toggle" || dispatcher == "hymission.toggle")
        return "hymission:toggle";
    if (dispatcher == "open" || dispatcher == "hymission.open")
        return "hymission:open";
    if (dispatcher == "close" || dispatcher == "hymission.close")
        return "hymission:close";
    if (dispatcher == "debug_current_layout" || dispatcher == "debugCurrentLayout" || dispatcher == "hymission.debug_current_layout" ||
        dispatcher == "hymission.debugCurrentLayout")
        return "hymission:debug_current_layout";
    if (dispatcher == "scroll" || dispatcher == "hymission.scroll")
        return "hymission:scroll";
    return dispatcher;
}

} // namespace hymission
