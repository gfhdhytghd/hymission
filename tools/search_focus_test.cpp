#include "search_focus.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

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
    // Exercise the production routing bridge. An unavailable helper stops
    // first, then the SAME key reaches overview; only overview decides whether
    // the event is consumed. Neither a focused nor a buffered key takes that path.
    const auto checkRoute = [](bool focused, bool escapePressed, bool overviewHandles) {
        bool searchActive = true;
        unsigned buffered = 1;
        bool cancelled = false;
        std::string calls;
        const auto route = routeSearchKey(searchKeyAction(focused, escapePressed), [&] {
            searchActive = false;
            buffered = 0;
            calls += 'S';
        }, [&] {
            ++buffered;
            cancelled = true;
            calls += 'B';
        });
        if (route == SearchKeyRoute::Overview) {
            if (searchActive || buffered != 0)
                return false;
            calls += 'O';
            if (overviewHandles)
                cancelled = true;
        }
        if (focused)
            return route == SearchKeyRoute::Helper && searchActive && !cancelled && calls.empty();
        if (!escapePressed)
            return route == SearchKeyRoute::Helper && searchActive && cancelled && buffered == 2 && calls == "B";
        return route == SearchKeyRoute::Overview && !searchActive && calls == "SO" && cancelled == overviewHandles;
    };
    if (!checkRoute(false, true, true) || !checkRoute(false, true, false) ||
        !checkRoute(false, false, true) || !checkRoute(true, true, true) || !checkRoute(true, false, true))
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
    std::cout << "PASS: search Escape resumes overview cancellation, bounded 8ms retries, focus loss and restart deadlines\n";
}
