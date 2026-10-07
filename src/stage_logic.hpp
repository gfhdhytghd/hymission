#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include "mission_layout.hpp"

namespace hymission::stage {

enum class OverviewPhase { Inactive, Active, Releasing };
enum class OverviewRenderOwner { Stage, Overview, PreparedStage };

// Ownership is a lifecycle decision, not an opacity/progress threshold. In
// particular, a zero-progress gesture is still owned by overview.
constexpr OverviewRenderOwner overviewRenderOwner(OverviewPhase phase, bool prepared) {
    if (phase == OverviewPhase::Inactive)
        return OverviewRenderOwner::Stage;
    if (phase == OverviewPhase::Releasing && prepared)
        return OverviewRenderOwner::PreparedStage;
    return OverviewRenderOwner::Overview;
}

// All dimensions are logical pixels. Width/height have already had the bar's
// reserved area removed; desktop gaps belong to the native layout, not here.
struct Settings {
    double minWidth = 120;
    double maxWidth = 0; // 0: one fifth of the output's logical width
    double padding = 12;
    double cardGap = 12;
    double desktopGap = 12;
    double paddingTop = -1;
    double paddingRight = -1;
    double paddingBottom = -1;
    bool evenSpacing = false; // stretch vertical gaps so the card stack spans the sidebar
};

struct Geometry {
    double cardWidth = 0;
    double cardHeight = 0;
    double bandWidth = 0;
    double reservation = 0;
    double desktopWidth = 0;
    double height = 0;
    double padding = 0;
    double paddingTop = 0;
    double paddingBottom = 0;
    double cardGap = 0;
    double maxScroll = 0;
    std::size_t count = 0;

    bool enabled() const { return count && reservation > 0; }
    double cardTop(std::size_t index, double scroll) const;
    double clampScroll(double scroll) const;
    double reveal(std::size_t index, double scroll) const;
    std::optional<std::size_t> hit(double x, double y, double scroll) const;
};

Settings normalize(Settings settings);
Geometry layout(double width, double height, std::size_t count, Settings settings,
                std::optional<double> frozenCardWidth = std::nullopt, double outputWidth = 0);
std::vector<WindowSlot> arrangeWindows(const std::vector<WindowInput>& windows, const Geometry& geometry, const Rect& desktop);
double previewRounding(double configured, double system, double width, double height);
double transitionProgress(double elapsed, double duration);
double staggeredProgress(double progress, std::size_t rank, std::size_t count, bool fromBottom);
// progress is the linear timeline, before either easing curve is applied.
Rect transitionBox(const Rect& from, const Rect& to, double progress);
Rect transitionBoxWithin(const Rect& from, const Rect& to, double progress, const Rect& bounds);
std::pair<double, double> mapDropPoint(const Rect& card, const Rect& desktop, double x, double y);
// Inverse of arrangeWindows' uniform preview transform, without pointer clamping.
std::pair<double, double> mapPreviewCenter(const Rect& card, const Rect& desktop, double x, double y);
Rect sidebarArea(const Rect& base, const Geometry& geometry, bool right);
Rect desktopArea(const Rect& base, const Geometry& geometry, bool right);

// The viewport is always a hard clip, including when both effect widths are 0.
// Widths are measured at the displayed size, independently of window geometry.
struct EdgeViewport {
    Rect box;
    double left = 0;
    double right = 0;
};
EdgeViewport edgeViewport(const Rect& box, double left, double right);
EdgeViewport desktopShadowViewport(const EdgeViewport& viewport, const Rect& output);
EdgeViewport edgeViewportForWindow(const EdgeViewport& viewport, const Rect& window);
double edgeOpacity(const EdgeViewport& viewport, double x, double y);
// Preserve both the gradient's coordinate system and the actual card/strip clip.
// A window fully outside clip must use an offscreen endpoint, not a reveal.
struct OverviewEndpoint {
    Rect window;
    EdgeViewport viewport;
    Rect clip;
};
struct EdgeFrame {
    EdgeViewport viewport;
    Rect clip;
};
double overviewTransitionProgress(double start, double current, bool opening);
EdgeFrame interpolateEdgeFrame(const EdgeFrame& from, const EdgeFrame& to, double progress);
bool overviewEndpointVisible(const OverviewEndpoint& endpoint);
EdgeFrame overviewRevealFrame(const OverviewEndpoint& endpoint, const Rect& currentWindow, const Rect& output, double openness);
struct ScrollingFlightFrame {
    Rect window;
    EdgeViewport viewport;
};
ScrollingFlightFrame scrollingFlightFrame(const Rect& from, const Rect& to, const EdgeViewport& fromViewport,
    const EdgeViewport& toViewport, double progress);

// Keep this policy independent of compositor enum values for state tests.
enum class CoverMode { None, Maximized, Fullscreen };
bool coversStrip(CoverMode mode, bool maximizeCover);

// Native window-list order is only meaningful within each rendering layer.
enum class PreviewLayer { Hidden, Tiled, Fullscreen, Floating };
PreviewLayer previewLayer(bool workspaceFullscreen, bool fullscreen, bool floating, bool allowedOverFullscreen);

} // namespace hymission::stage
