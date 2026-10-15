#include "config_command.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}

struct Call {
    std::string command, args, result;
};

// Assert the full command sequence as well as each argument, not only success.
std::string run(hymission::ConfigCommandAdapter& adapter, const std::string& name, const std::string& value, const std::vector<Call>& calls) {
    std::size_t next = 0;
    const auto result = adapter.set(name, value, [&](const std::string& command, const std::string& args) {
        require(next < calls.size(), "unexpected IPC command: " + command);
        const auto& expected = calls[next++];
        require(command == expected.command && args == expected.args, "wrong command/arguments: " + command + " " + args);
        return expected.result;
    });
    require(next == calls.size(), "missing IPC command");
    return result;
}

std::string expr(const std::string& name, const std::string& value) {
    return hymission::luaConfigExpression(name, value);
}
}

int main() {
    using hymission::ConfigCommandAdapter;
    require(expr("scrolling:follow_focus", "0") == "hl.config({[\"scrolling\"] = {[\"follow_focus\"] = 0}})", "Lua nesting");
    require(expr("input.follow_mouse", "2") == "hl.config({[\"input\"] = {[\"follow_mouse\"] = 2}})", "dot separator/Int");
    require(expr("a\"b:c\\d", "1") == "hl.config({[\"a\\\"b\"] = {[\"c\\\\d\"] = 1}})", "Lua key escaping");

    // Legacy successes never need eval, including every disable/restore value
    // used by the controller (Bool 0/1 and Int modes 0/1/2/3).
    for (const auto& name : {"input:follow_mouse", "scrolling:follow_focus", "animations:enabled", "debug:damage_tracking"}) {
        for (const auto& value : {"0", "1", "2", "3"}) {
            if ((std::string(name) == "scrolling:follow_focus" || std::string(name) == "animations:enabled") && std::string(value) > "1")
                continue;
            if (std::string(name) == "debug:damage_tracking" && std::string(value) == "3")
                continue;
            ConfigCommandAdapter legacy;
            require(run(legacy, name, value, {{"keyword", std::string(name) + " " + value, "ok"}}).empty(), "legacy write");
        }
    }

    // v0.56 Lua rejection, removed command, arbitrary failures and empty replies
    // must all fall back, with a cached eval-only restoration afterwards.
    for (const auto& failure : {"keyword can't work with non-legacy parsers. Use eval.", "unknown request", "", "invalid value", "ok\n"}) {
        for (const auto& name : {"input:follow_mouse", "scrolling:follow_focus", "animations:enabled", "debug:damage_tracking"}) {
            ConfigCommandAdapter lua;
            require(run(lua, name, "0", {{"keyword", std::string(name) + " 0", failure}, {"eval", expr(name, "0"), "ok"}}).empty(), "fallback");
            require(run(lua, name, "1", {{"eval", expr(name, "1"), "ok"}}).empty(), "cached Bool restore");
            // A reload can switch Lua -> legacy: it must probe keyword again.
            lua.reset();
            require(run(lua, name, "1", {{"keyword", std::string(name) + " 1", "ok"}}).empty(), "reload invalidates cache");
        }
    }

    ConfigCommandAdapter adapter;
    require(run(adapter, "debug:damage_tracking", "0", {{"keyword", "debug:damage_tracking 0", "missing"}, {"eval", expr("debug:damage_tracking", "0"), "ok"}}).empty(), "prime eval cache");
    require(run(adapter, "debug:damage_tracking", "2", {{"eval", expr("debug:damage_tracking", "2"), "ok"}}).empty(), "cached Int restore");
    // If cached eval stops working, try keyword during this same write.
    require(run(adapter, "input:follow_mouse", "2", {{"eval", expr("input:follow_mouse", "2"), "no Lua provider"}, {"keyword", "input:follow_mouse 2", "ok"}}).empty(), "provider changed without reload");
    require(run(adapter, "input:follow_mouse", "0", {{"keyword", "input:follow_mouse 0", "ok"}}).empty(), "keyword preference recovered");

    auto error = run(adapter, "animations:enabled", "0", {{"keyword", "animations:enabled 0", "keyword failed"}, {"eval", expr("animations:enabled", "0"), "eval failed"}});
    require(error == "keyword: keyword failed; eval: eval failed", "both errors retained");
    require(run(adapter, "animations:enabled", "1", {{"keyword", "animations:enabled 1", "ok"}}).empty(), "failed fallback not cached");
    error = run(adapter, "animations:enabled", "0", {{"keyword", "animations:enabled 0", ""}, {"eval", expr("animations:enabled", "0"), ""}});
    require(error == "keyword: <empty response>; eval: <empty response>", "empty replies must not mean success");

    require(run(adapter, "animations:enabled", "0", {{"keyword", "animations:enabled 0", "missing"}, {"eval", expr("animations:enabled", "0"), "ok"}}).empty(), "re-prime cache");
    error = run(adapter, "animations:enabled", "1", {{"eval", expr("animations:enabled", "1"), "cached eval failed"}, {"keyword", "animations:enabled 1", "keyword failed"}});
    require(error == "keyword: keyword failed; eval: cached eval failed", "cached-path diagnostics");
    require(run(adapter, "animations:enabled", "1", {{"keyword", "animations:enabled 1", "ok"}}).empty(), "failed cached path invalidates cache");

    error = run(adapter, ":.", "0", {{"keyword", ":. 0", "bad name"}});
    require(error == "keyword: bad name; eval: no Lua expression", "no expression diagnostic");
    std::cout << "PASS: legacy/Lua/missing keyword, cache, reload/provider switch, errors and Bool/Int disable/restore\n";
}
