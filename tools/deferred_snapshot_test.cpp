#include "deferred_snapshot.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    hymission::DeferredSnapshotRefresh refresh;
    unsigned captures = 0;
    const auto capture = [&](bool rendering) {
        if (refresh.take(rendering))
            ++captures;
    };
    if (refresh.pending())
        return EXIT_FAILURE;
    // Multiple requests while collecting a frame coalesce into one capture.
    for (unsigned i = 0; i < 100; ++i) {
        refresh.request();
        capture(true);
    }
    if (!refresh.pending() || captures != 0)
        return EXIT_FAILURE;
    // Nested dispatch cannot consume the request; the later frame-free callback can.
    capture(true);
    capture(false);
    capture(false);
    if (refresh.pending() || captures != 1)
        return EXIT_FAILURE;
    refresh.request();
    refresh.reset(); // close/unload cancels pending work
    capture(false);
    if (refresh.pending() || captures != 1)
        return EXIT_FAILURE;
    std::cout << "PASS: deferred captures coalesce, wait for frame exit and cancel on teardown\n";
}
