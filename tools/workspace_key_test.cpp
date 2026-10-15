#include "workspace_key.hpp"
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
}

// The target headers initialize native logger/color objects. Resolve them from
// the matching binary's --version path, as the framebuffer contract test does.
__attribute__((constructor)) static void checkWorkspaceKeyContract() {
    if (!std::getenv("HYMISSION_WORKSPACE_KEY_TEST"))
        return;
    using namespace hymission;
    using Workspace::SWorkspaceNumberedID;
    using Workspace::SWorkspaceSpecialID;
    using Workspace::eWorkspaceType;
    using State::Workspace::STarget;

    check(workspaceKey(STarget{}) == WORKSPACE_INVALID, "invalid target stays invalid");
    check(!workspaceTarget(WORKSPACE_INVALID).valid() && !workspaceTarget(WORKSPACE_NOT_CHANGED).valid(), "sentinels cannot create workspaces");
    check(!workspaceTarget(0).valid() && !workspaceTarget(INT64_MAX).valid(), "numbered IDs stay within the native range");
    const STarget numbered{SWorkspaceNumberedID{7}, "7", "renamed seven", eWorkspaceType::NORMAL};
    check(workspaceKey(numbered) == 7 && workspaceTarget(7).id == numbered.id, "numbered slots retain their actual number");
    check(workspaceKey(STarget{SWorkspaceNumberedID{UINT32_MAX}, std::to_string(UINT32_MAX), {}, eWorkspaceType::NORMAL}) == UINT32_MAX,
          "largest native numbered ID remains valid");

    const STarget named{SWorkspaceSpecialID{}, "code", "Code", eWorkspaceType::NORMAL};
    const STarget special{SWorkspaceSpecialID{}, "special:code", "Code", eWorkspaceType::SPECIAL};
    const auto namedKey = workspaceKey(named);
    const auto specialKey = workspaceKey(special);
    check(namedKey < 0 && specialKey < 0 && namedKey != specialKey, "unnumbered identities cannot collide");
    const auto nextKey = workspaceKey(STarget{SWorkspaceSpecialID{}, "chat", "Chat", eWorkspaceType::NORMAL});
    check(nextKey < namedKey, "reverse creation order stays ahead of numbered slots");
    check(workspaceKey(STarget{SWorkspaceSpecialID{}, "code", "Mission Control", eWorkspaceType::NORMAL}) == namedKey,
          "temporary display rename preserves identity");
    check(workspaceTarget(namedKey).address == "code" && workspaceTarget(namedKey).displayName == "Mission Control",
          "lookups and recreation use the address, with the latest display name");
    check(workspaceKey(STarget{SWorkspaceSpecialID{}, "code", "Code", eWorkspaceType::NORMAL}) == namedKey,
          "restoring or recreating a named workspace retains its key");
    const auto sameAddressOtherType = workspaceKey(STarget{SWorkspaceSpecialID{}, "code", "Code", eWorkspaceType::SPECIAL});
    check(sameAddressOtherType != namedKey && workspaceTarget(sameAddressOtherType).type == eWorkspaceType::SPECIAL,
          "workspace type participates in unnumbered identity");
    check(!workspaceTarget(INT64_MIN).valid(), "out-of-range negative keys cannot resolve");
    std::puts("PASS: workspace identity, ordering, renames, sentinels and numbered slot bounds");
}
