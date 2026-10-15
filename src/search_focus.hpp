#pragma once

#include <chrono>
#include <optional>

namespace hymission {

enum class SearchKeyAction { Forward, Buffer, Stop };
enum class SearchKeyRoute { Helper, Overview };

// Escape must remain a way out while a spawned helper is not yet usable.
inline SearchKeyAction searchKeyAction(bool focused, bool escapePressed) {
    if (focused)
        return SearchKeyAction::Forward;
    return escapePressed ? SearchKeyAction::Stop : SearchKeyAction::Buffer;
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
