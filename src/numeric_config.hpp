#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace hymission {

// Validate at the read boundary: host plugin adapters need not retain ranges.
inline double finiteConfig(double value, double fallback, double minimum, double maximum) {
    return std::clamp(std::isfinite(value) ? value : fallback, minimum, maximum);
}

inline std::optional<float> gestureScale(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const float value = std::stof(text, &consumed);
        if (consumed != text.size() || !std::isfinite(value))
            return std::nullopt;
        return std::clamp(value, 0.1F, 10.F);
    } catch (...) {
        return std::nullopt;
    }
}

inline std::optional<std::size_t> gestureFingerCount(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        return std::nullopt;
    try {
        const auto value = std::stoul(text);
        if (value >= 2 && value <= 9)
            return value;
    } catch (...) {
    }
    return std::nullopt;
}

} // namespace hymission
