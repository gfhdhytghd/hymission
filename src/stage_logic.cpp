#include "stage_logic.hpp"

#include <algorithm>
#include <cmath>

namespace hymission::stage {
namespace {
double sane(double value, double fallback) { return std::isfinite(value) ? value : fallback; }
}

Settings normalize(Settings s) {
    s.minWidth = std::max(8.0, sane(s.minWidth, 120));
    s.maxWidth = sane(s.maxWidth, 0);
    s.maxWidth = s.maxWidth <= 0 ? 0 : std::max(s.minWidth, s.maxWidth);
    s.padding = std::max(0.0, sane(s.padding, 12));
    s.paddingTop = s.paddingTop < 0 ? s.padding : std::max(0.0, sane(s.paddingTop, s.padding));
    s.paddingRight = s.paddingRight < 0 ? s.padding : std::max(0.0, sane(s.paddingRight, s.padding));
    s.paddingBottom = s.paddingBottom < 0 ? s.padding : std::max(0.0, sane(s.paddingBottom, s.padding));
    s.cardGap = std::max(0.0, sane(s.cardGap, 12));
    s.desktopGap = std::max(0.0, sane(s.desktopGap, 12));
    return s;
}

Geometry layout(double width, double height, std::size_t count, Settings settings, std::optional<double> frozenCardWidth, double outputWidth) {
    Geometry result;
    result.desktopWidth = std::max(0.0, sane(width, 0));
    result.height = std::max(0.0, sane(height, 0));
    const auto s = normalize(settings);
    // A 64px usable desktop and an 8px card are hard floors only on tiny outputs.
    const double availableWidth = width - s.padding - s.paddingRight - s.desktopGap;
    const double maximum = s.maxWidth > 0 ? s.maxWidth : (outputWidth > 0 ? outputWidth : width) / 5;
    const double upper = std::min(maximum, availableWidth - 64);
    if (!count || !std::isfinite(width) || !std::isfinite(height) || upper < 8 || height <= s.paddingTop + s.paddingBottom + 8)
        return result;

    const double lower = std::min(s.minWidth, upper);
    const double availableHeight = height - s.paddingTop - s.paddingBottom - static_cast<double>(count - 1) * s.cardGap;
    // Solve N*c*H/(W-2p-g-c) <= H-2p-(N-1)*gap directly. The desktop
    // width and the card width are coupled; fitting against the full monitor
    // aspect first would produce the wrong ratio (and sometimes oscillate).
    const double fit = availableHeight > 0 ? availableHeight * availableWidth / (static_cast<double>(count) * height + availableHeight) : lower;
    const double c = std::clamp(frozenCardWidth ? sane(*frozenCardWidth, fit) : fit, lower, upper);
    result.cardWidth = c;
    result.bandWidth = c + s.padding + s.paddingRight;
    result.reservation = result.bandWidth + s.desktopGap;
    result.desktopWidth = width - result.reservation;
    result.cardHeight = c * height / result.desktopWidth;
    result.padding = s.padding;
    result.paddingTop = s.paddingTop;
    result.paddingBottom = s.paddingBottom;
    result.cardGap = s.cardGap;
    // Cards capped below the fit width leave the column short. With even
    // spacing, distribute the full leftover height into the gaps so the stack
    // spans the sidebar evenly instead of clustering at the top.
    if (s.evenSpacing && count > 1) {
        const double used = static_cast<double>(count) * result.cardHeight;
        const double leftover = height - s.paddingTop - s.paddingBottom - used;
        if (leftover > 0)
            result.cardGap = std::max(s.cardGap, leftover / static_cast<double>(count - 1));
    }
    result.count = count;
    result.maxScroll = std::max(0.0, static_cast<double>(count) * result.cardHeight + static_cast<double>(count - 1) * s.cardGap + s.paddingTop + s.paddingBottom - height);
    return result;
}

double Geometry::clampScroll(double scroll) const { return std::clamp(sane(scroll, 0), 0.0, maxScroll); }
double Geometry::cardTop(std::size_t index, double scroll) const { return paddingTop + static_cast<double>(index) * (cardHeight + cardGap) - clampScroll(scroll); }

double Geometry::reveal(std::size_t index, double scroll) const {
    scroll = clampScroll(scroll);
    if (index >= count)
        return scroll;
    const auto top = cardTop(index, scroll);
    if (top < paddingTop)
        return clampScroll(scroll + top - paddingTop);
    if (top + cardHeight > height - paddingBottom)
        return clampScroll(scroll + top + cardHeight - height + paddingBottom);
    return scroll;
}

std::optional<std::size_t> Geometry::hit(double x, double y, double scroll) const {
    if (!enabled() || !std::isfinite(x) || !std::isfinite(y) || x < padding || x >= padding + cardWidth || y < paddingTop || y >= height - paddingBottom)
        return std::nullopt;
    const double relative = y + clampScroll(scroll) - paddingTop;
    if (relative < 0)
        return std::nullopt;
    const auto index = static_cast<std::size_t>(relative / (cardHeight + cardGap));
    if (index >= count || relative - static_cast<double>(index) * (cardHeight + cardGap) >= cardHeight)
        return std::nullopt;
    return index;
}

bool coversStrip(CoverMode mode, bool maximizeCover) {
    return mode == CoverMode::Fullscreen || (mode == CoverMode::Maximized && maximizeCover);
}

PreviewLayer previewLayer(bool workspaceFullscreen, bool fullscreen, bool floating, bool allowedOverFullscreen) {
    if (fullscreen)
        return PreviewLayer::Fullscreen;
    if (workspaceFullscreen && !(floating && allowedOverFullscreen))
        return PreviewLayer::Hidden;
    return floating ? PreviewLayer::Floating : PreviewLayer::Tiled;
}

std::vector<WindowSlot> arrangeWindows(const std::vector<WindowInput>& windows, const Geometry& g, const Rect& desktop) {
    std::vector<WindowSlot> slots;
    if (desktop.width <= 0 || desktop.height <= 0 || g.cardWidth <= 0 || g.cardHeight <= 0)
        return slots;
    const double scale = std::min(g.cardWidth / desktop.width, g.cardHeight / desktop.height);
    const double x = (g.cardWidth - desktop.width * scale) / 2;
    const double y = (g.cardHeight - desktop.height * scale) / 2;
    // One transform for the entire workspace preserves placement, overlap and
    // stacking order. Off-desktop portions are clipped by the card framebuffer.
    for (const auto& window : windows) {
        const auto& box = window.natural;
        slots.push_back(WindowSlot{.index = window.index, .natural = box,
            .target = {x + (box.x - desktop.x) * scale, y + (box.y - desktop.y) * scale, box.width * scale, box.height * scale}, .scale = scale});
    }
    return slots;
}

double previewRounding(double configured, double system, double width, double height) {
    const double radius = configured < 0 ? std::max(0.0, sane(system, 0)) / 2 : sane(configured, 0);
    return std::clamp(radius, 0.0, std::max(0.0, std::min(width, height) / 2));
}

double transitionProgress(double elapsed, double duration) {
    if (duration <= 0)
        return 1;
    const double t = std::clamp(sane(elapsed / duration, 1), 0.0, 1.0);
    return 1 - std::pow(1 - t, 3);
}

double staggeredProgress(double progress, std::size_t rank, std::size_t count, bool fromBottom) {
    const auto index = count > 0 ? std::min(rank, count - 1) : 0;
    const auto ordered = fromBottom && count > 0 ? count - 1 - index : index;
    const double delay = count > 1 ? 0.35 * ordered / (count - 1) : 0;
    return transitionProgress(std::max(0.0, progress - delay), 1 - delay);
}

Rect transitionBox(const Rect& from, const Rect& to, double p) {
    p = std::clamp(sane(p, 1), 0.0, 1.0);
    const double scale = transitionProgress(p, 1);
    // Growing windows move all four corners along straight segments. Use the
    // same easing for position and size; retain the outgoing shrink curve.
    const bool growing = to.width > from.width || to.height > from.height;
    const double position = growing ? scale : p * p * (3 - 2 * p);
    const double width = from.width + (to.width - from.width) * scale;
    const double height = from.height + (to.height - from.height) * scale;
    const double centerX = from.x + from.width / 2 + (to.x + to.width / 2 - from.x - from.width / 2) * position;
    const double centerY = from.y + from.height / 2 + (to.y + to.height / 2 - from.y - from.height / 2) * position;
    return {centerX - width / 2, centerY - height / 2, width, height};
}
Rect transitionBoxWithin(const Rect& from, const Rect& to, double progress, const Rect& bounds) {
    if (bounds.width <= 0 || bounds.height <= 0)
        return {bounds.x, bounds.y, 0, 0};
    const auto fitWithin = [&](Rect box) {
        const double fit = std::min({1.0, bounds.width / std::max(1.0, box.width), bounds.height / std::max(1.0, box.height)});
        const double centerX = box.centerX();
        const double centerY = box.centerY();
        box.width *= fit;
        box.height *= fit;
        box.x = std::clamp(centerX - box.width / 2, bounds.x, bounds.x + bounds.width - box.width);
        box.y = std::clamp(centerY - box.height / 2, bounds.y, bounds.y + bounds.height - box.height);
        return box;
    };
    if (to.width > from.width || to.height > from.height) {
        // Constrain endpoints first, avoiding a bent path from per-frame clamps.
        const auto start = fitWithin(from);
        const auto end = fitWithin(to);
        const double p = transitionProgress(progress, 1);
        return {start.x + (end.x - start.x) * p, start.y + (end.y - start.y) * p,
                start.width + (end.width - start.width) * p, start.height + (end.height - start.height) * p};
    }
    return fitWithin(transitionBox(from, to, progress));
}

std::pair<double, double> mapDropPoint(const Rect& card, const Rect& desktop, double x, double y) {
    const double u = card.width > 0 ? std::clamp((x - card.x) / card.width, 0.0, 1.0) : 0.5;
    const double v = card.height > 0 ? std::clamp((y - card.y) / card.height, 0.0, 1.0) : 0.5;
    return {desktop.x + u * desktop.width, desktop.y + v * desktop.height};
}
std::pair<double, double> mapPreviewCenter(const Rect& card, const Rect& desktop, double x, double y) {
    if (card.width <= 0 || card.height <= 0 || desktop.width <= 0 || desktop.height <= 0)
        return {desktop.centerX(), desktop.centerY()};
    const double scale = std::min(card.width / desktop.width, card.height / desktop.height);
    return {desktop.x + (x - card.x - (card.width - desktop.width * scale) / 2) / scale,
            desktop.y + (y - card.y - (card.height - desktop.height * scale) / 2) / scale};
}
Rect sidebarArea(const Rect& base, const Geometry& geometry, bool right) {
    return {right ? base.x + base.width - geometry.bandWidth : base.x, base.y, geometry.bandWidth, base.height};
}

Rect desktopArea(const Rect& base, const Geometry& geometry, bool right) {
    return {right ? base.x : base.x + geometry.reservation, base.y, geometry.desktopWidth, base.height};
}
EdgeViewport edgeViewport(const Rect& box, double left, double right) {
    const double width = std::max(0.0, sane(box.width, 0));
    left = std::clamp(sane(left, 0), 0.0, width);
    right = std::clamp(sane(right, 0), 0.0, width);
    if (left + right > width) {
        const double scale = width / (left + right);
        left *= scale;
        right *= scale;
    }
    return {box, left, right};
}

EdgeViewport desktopShadowViewport(const EdgeViewport& viewport, const Rect& output) {
    auto result = viewport;
    result.box.y = output.y;
    result.box.height = output.height;
    return result;
}

EdgeViewport edgeViewportForWindow(const EdgeViewport& viewport, const Rect& window) {
    auto result = viewport;
    if (window.x >= viewport.box.x - 0.01)
        result.left = 0;
    if (window.x + window.width <= viewport.box.x + viewport.box.width + 0.01)
        result.right = 0;
    return result;
}

double edgeOpacity(const EdgeViewport& viewport, double x, double y) {
    const auto& b = viewport.box;
    if (x < b.x || x >= b.x + b.width || y < b.y || y >= b.y + b.height || b.width <= 0 || b.height <= 0)
        return 0;
    const auto smooth = [](double t) { t = std::clamp(t, 0.0, 1.0); return t * t * (3 - 2 * t); };
    return std::min(viewport.left > 0 ? smooth(2 * (x - b.x) / viewport.left) : 1.0,
        viewport.right > 0 ? smooth(2 * (b.x + b.width - x) / viewport.right) : 1.0);
}

double overviewTransitionProgress(double start, double current, bool opening) {
    const double distance = opening ? 1 - start : start;
    return distance > 1e-9 ? std::clamp((opening ? current - start : start - current) / distance, 0.0, 1.0) : 1.0;
}

static EdgeFrame pinOuterEdge(EdgeFrame frame, const EdgeViewport& anchor) {
    if (anchor.fixedOuterEdge == FixedOuterEdge::None)
        return frame;
    const auto pin = [&](Rect& box) {
        if (anchor.fixedOuterEdge == FixedOuterEdge::Left) {
            const double right = box.x + box.width;
            box.x = anchor.fixedOuterX;
            box.width = std::max(0.0, right - box.x);
        } else
            box.width = std::max(0.0, anchor.fixedOuterX - box.x);
    };
    pin(frame.viewport.box);
    pin(frame.clip);
    frame.viewport.fixedOuterEdge = anchor.fixedOuterEdge;
    frame.viewport.fixedOuterX = anchor.fixedOuterX;
    return frame;
}

EdgeFrame interpolateEdgeFrame(const EdgeFrame& from, const EdgeFrame& to, double progress) {
    const double p = std::clamp(sane(progress, 0), 0.0, 1.0);
    const double t = p * p * (3 - 2 * p);
    const auto mix = [t](double a, double b) { return a + (b - a) * t; };
    const auto mixRect = [&](const Rect& a, const Rect& b) {
        return Rect{mix(a.x, b.x), mix(a.y, b.y), mix(a.width, b.width), mix(a.height, b.height)};
    };
    const EdgeFrame frame{edgeViewport(mixRect(from.viewport.box, to.viewport.box), mix(from.viewport.left, to.viewport.left),
                                      mix(from.viewport.right, to.viewport.right)), mixRect(from.clip, to.clip)};
    // Releasing/reversing a gesture retains the sampled outer edge, even when
    // the other endpoint is the unrestricted overview output.
    return pinOuterEdge(frame, from.viewport.fixedOuterEdge != FixedOuterEdge::None ? from.viewport : to.viewport);
}

bool overviewEndpointVisible(const OverviewEndpoint& endpoint) {
    const auto& w = endpoint.window;
    const auto& c = endpoint.clip;
    return w.width > 0 && w.height > 0 && c.width > 0 && c.height > 0 &&
        std::min(w.x + w.width, c.x + c.width) > std::max(w.x, c.x) &&
        std::min(w.y + w.height, c.y + c.height) > std::max(w.y, c.y);
}

EdgeFrame overviewRevealFrame(const OverviewEndpoint& endpoint, const Rect& current, const Rect& output, double openness) {
    const double p = std::clamp(sane(openness, 0), 0.0, 1.0);
    const double reveal = p * p * (3 - 2 * p);
    const double sx = current.width / std::max(1.0, endpoint.window.width);
    const double sy = current.height / std::max(1.0, endpoint.window.height);
    const auto unfold = [&](const Rect& rect) {
        const Rect mapped{current.x + (rect.x - endpoint.window.x) * sx,
                          current.y + (rect.y - endpoint.window.y) * sy, rect.width * sx, rect.height * sy};
        return Rect{mapped.x + (output.x - mapped.x) * reveal, mapped.y + (output.y - mapped.y) * reveal,
                    mapped.width + (output.width - mapped.width) * reveal, mapped.height + (output.height - mapped.height) * reveal};
    };
    const auto viewport = edgeViewport(unfold(endpoint.viewport.box), endpoint.viewport.left * sx * (1 - reveal),
                                      endpoint.viewport.right * sx * (1 - reveal));
    return pinOuterEdge({viewport, unfold(endpoint.clip)}, endpoint.viewport);
}

ScrollingFlightFrame scrollingFlightFrame(const Rect& from, const Rect& to, const EdgeViewport& fromViewport,
    const EdgeViewport& toViewport, double progress) {
    const double t = transitionProgress(progress, 1);
    const auto lerp = [t](double a, double b) { return a + (b - a) * t; };
    const auto& a = fromViewport.box;
    const auto& b = toViewport.box;
    const Rect view{lerp(a.x, b.x), lerp(a.y, b.y), lerp(a.width, b.width), lerp(a.height, b.height)};
    const auto viewport = edgeViewport(view, lerp(fromViewport.left, toViewport.left), lerp(fromViewport.right, toViewport.right));
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0)
        return {{view.x, view.y, 0, 0}, viewport};
    // Interpolate relative coordinates so hidden tape columns stay hidden.
    // The renderer intersects this viewport with output bounds; fitting either
    // rectangle would move/resize native content at the animation endpoints.
    return {{view.x + lerp((from.x - a.x) / a.width, (to.x - b.x) / b.width) * view.width,
             view.y + lerp((from.y - a.y) / a.height, (to.y - b.y) / b.height) * view.height,
             lerp(from.width / a.width, to.width / b.width) * view.width,
             lerp(from.height / a.height, to.height / b.height) * view.height}, viewport};
}
ScrollingFlightFrame edgeTransferFrame(const Rect& from, const Rect& to, const EdgeViewport& fromViewport,
    const EdgeViewport& toViewport, double progress) {
    const double p = std::clamp(sane(progress, 1), 0.0, 1.0);
    const auto& a = fromViewport.box;
    const auto& b = toViewport.box;
    if (std::abs(a.x - b.x) < 0.01 && std::abs(a.y - b.y) < 0.01 &&
        std::abs(a.width - b.width) < 0.01 && std::abs(a.height - b.height) < 0.01)
        return {transitionBox(from, to, p), toViewport};

    const bool entering = p >= 0.5;
    const auto& viewport = entering ? toViewport : fromViewport;
    const auto& view = viewport.box;
    const auto& endpoint = entering ? to : from;
    // Never sweep an already hidden scrolling column across the visible region.
    if (endpoint.x + endpoint.width <= view.x || endpoint.x >= view.x + view.width ||
        endpoint.y + endpoint.height <= view.y || endpoint.y >= view.y + view.height)
        return {endpoint, viewport};
    auto hidden = endpoint;
    if (std::abs(a.centerX() - b.centerX()) > 0.01) {
        const bool left = entering ? a.centerX() < b.centerX() : b.centerX() < a.centerX();
        hidden.x = left ? view.x - endpoint.width : view.x + view.width;
    } else {
        const bool top = entering ? a.centerY() < b.centerY() : b.centerY() < a.centerY();
        hidden.y = top ? view.y - endpoint.height : view.y + view.height;
    }
    return {entering ? transitionBox(hidden, endpoint, (p - 0.5) * 2) : transitionBox(endpoint, hidden, p * 2), viewport};
}
} // namespace hymission::stage
