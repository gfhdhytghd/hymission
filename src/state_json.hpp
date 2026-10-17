#pragma once

#include "vendor/nlohmann/json.hpp"

namespace hymission {

// X11 titles/classes and workspace names may contain arbitrary byte strings.
// Keep the state response valid even when one field is not valid UTF-8.
inline std::string dumpStateJson(const nlohmann::json& state) {
    return state.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace hymission
