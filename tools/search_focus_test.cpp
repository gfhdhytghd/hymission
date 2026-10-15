#include "search_focus.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    using namespace hymission;
    using namespace std::chrono_literals;
    const SearchFocusWait::Clock::time_point start{};
    SearchFocusWait wait;
    wait.begin(start);
    unsigned retries = 0;
    // An alive but unmapped helper (or a mapped helper denied focus) must
    // exhaust the same total deadline even when every 8ms retry calls begin.
    for (auto elapsed = 0ms; elapsed < 2s; elapsed += std::chrono::milliseconds(SearchFocusWait::RETRY_MS)) {
        wait.begin(start + elapsed);
        if (wait.expired(start + elapsed))
            break;
        ++retries;
    }
    if (retries != 188 || !wait.expired(start + 1500ms) || wait.expired(start + 1499ms))
        return EXIT_FAILURE;
    if (searchKeyAction(false, true) != SearchKeyAction::Stop ||
        searchKeyAction(false, false) != SearchKeyAction::Buffer ||
        searchKeyAction(true, true) != SearchKeyAction::Forward ||
        searchKeyAction(true, false) != SearchKeyAction::Forward)
        return EXIT_FAILURE;
    wait.reset(); // successful focus or stop cancels the old deadline
    if (wait.expired(start + 10s))
        return EXIT_FAILURE;
    wait.begin(start + 10s); // a later focus loss gets one new acquisition budget
    if (wait.expired(start + 11499ms) || !wait.expired(start + 11500ms))
        return EXIT_FAILURE;
    wait.reset();
    wait.begin(start + 20s); // reopening must not inherit an expired attempt
    if (wait.expired(start + 20s))
        return EXIT_FAILURE;
    std::cout << "PASS: search Escape routing, bounded 8ms retries, focus loss and restart deadlines\n";
}
