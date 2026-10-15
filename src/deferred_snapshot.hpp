#pragma once

namespace hymission {

// A busy callback leaves work pending. Only an external frame/event may queue
// another callback, so nested event-loop dispatch cannot form an idle spin.
class DeferredSnapshotRefresh {
  public:
    void request() { m_pending = true; }
    [[nodiscard]] bool pending() const { return m_pending; }
    void reset() { m_pending = false; }
    [[nodiscard]] bool take(bool rendering) {
        if (rendering || !m_pending)
            return false;
        m_pending = false;
        return true;
    }

  private:
    bool m_pending = false;
};

} // namespace hymission
