#pragma once

#include <hyprland/src/helpers/math/CornerRadii.hpp>

namespace hymission {

template <class Transform>
Math::SCornerRadii mapCornerRadii(const Math::SCornerRadii& radii, Transform transform) {
    return {
        static_cast<float>(transform(radii.topLeft)),
        static_cast<float>(transform(radii.topRight)),
        static_cast<float>(transform(radii.bottomRight)),
        static_cast<float>(transform(radii.bottomLeft)),
    };
}

} // namespace hymission
