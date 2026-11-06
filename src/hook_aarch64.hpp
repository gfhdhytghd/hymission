#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Function hooks for AArch64, where Hyprland's own CFunctionHook refuses to work.
// The plugin runs inside the compositor, so it can patch the entry itself.
namespace hymission::hooks::aarch64 {
struct Hook;
Hook* create(void* source, void* destination);
// False when the entry cannot be patched safely; the source is left untouched.
bool attach(Hook* hook);
void detach(Hook* hook);
// The callable original while attached, otherwise nullptr.
void* original(const Hook* hook);
void destroy(Hook* hook);
// Code that runs the first `count` instructions at `source` from `address`, then
// resumes the original. Empty when those instructions cannot be moved.
std::vector<uint8_t> relocate(const uint32_t* source, size_t count, uint64_t address);
}
