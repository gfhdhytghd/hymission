#include "input_guard.hpp"

#include <stdexcept>
#include <iostream>

struct LockManager {
    bool locked = false;
    unsigned queries = 0;
    bool isSessionLocked() { ++queries; return locked; }
};

void require(bool ok) { if (!ok) throw std::runtime_error("session lock predicate"); }

int main() {
    LockManager manager;
    require(!hymission::sessionLocked(static_cast<LockManager*>(nullptr)));
    require(!hymission::sessionLocked(&manager));
    manager.locked = true;
    require(hymission::sessionLocked(&manager));
    manager.locked = false;
    require(!hymission::sessionLocked(&manager));
    require(manager.queries == 3);
    std::cout << "PASS: shared session lock predicate, absent manager and unlock\n";
}
