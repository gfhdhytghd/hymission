#pragma once
#include <optional>
#include <utility>

namespace hymission::hooks {
// Optional capabilities are probed once per config generation, including failure.
class AttemptCache {
    std::optional<bool> result;
  public:
    template<class F> bool run(F&& attempt) {
        if (!result) result=std::forward<F>(attempt)();
        return *result;
    }
    void reset() { result.reset(); }
    void release() { if (result.value_or(false)) reset(); }
};
}
