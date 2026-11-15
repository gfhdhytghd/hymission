#pragma once

#include <chrono>
#include <optional>

namespace hymission {

enum class SearchKeyAction { Forward, Buffer, Stop, Pass };
enum class SearchKeyRoute { Helper, Overview };

template <typename Range, typename MatchesKey, typename IsPressed>
inline bool hasBufferedSearchPress(const Range& keys, MatchesKey matchesKey, IsPressed isPressed) {
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        if (matchesKey(*it))
            return isPressed(*it);
    }
    return false;
}

// Escape must remain a way out while a spawned helper is not yet usable.
inline SearchKeyAction searchKeyAction(bool focused, bool escapePressed, bool unpairedRelease) {
    if (focused)
        return SearchKeyAction::Forward;
    if (escapePressed)
        return SearchKeyAction::Stop;
    return unpairedRelease ? SearchKeyAction::Pass : SearchKeyAction::Buffer;
}

// Stopping an unavailable helper must resume overview handling of this same
// event. Returning directly to the compositor would leak it to the client below.
template <typename StopSearch, typename BufferKey>
[[nodiscard]] SearchKeyRoute routeSearchKey(SearchKeyAction action, StopSearch stopSearch, BufferKey bufferKey) {
    switch (action) {
        case SearchKeyAction::Stop:
            stopSearch();
            return SearchKeyRoute::Overview;
        case SearchKeyAction::Buffer:
            bufferKey();
            return SearchKeyRoute::Helper;
        case SearchKeyAction::Forward:
            return SearchKeyRoute::Helper;
        case SearchKeyAction::Pass:
            return SearchKeyRoute::Overview;
    }
    return SearchKeyRoute::Overview;
}

class SearchFocusWait {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto TIMEOUT = std::chrono::milliseconds(1500);
    static constexpr int RETRY_MS = 8;

    void begin(Clock::time_point now) {
        if (!m_deadline)
            m_deadline = now + TIMEOUT;
    }
    void reset() { m_deadline.reset(); }
    [[nodiscard]] bool expired(Clock::time_point now) const { return m_deadline && now >= *m_deadline; }

  private:
    std::optional<Clock::time_point> m_deadline;
};

} // namespace hymission
