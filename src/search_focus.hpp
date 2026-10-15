#pragma once

#include <chrono>
#include <optional>

namespace hymission {

enum class SearchKeyAction { Forward, Buffer, Stop };

// Escape must remain a way out while a spawned helper is not yet usable.
inline SearchKeyAction searchKeyAction(bool focused, bool escapePressed) {
    if (focused)
        return SearchKeyAction::Forward;
    return escapePressed ? SearchKeyAction::Stop : SearchKeyAction::Buffer;
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
