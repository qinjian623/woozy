#pragma once
#include <cstdint>

// Hosted startup ABI.  These helpers only publish the original process
// argument/environment vectors to the existing runtime; they never install an
// FS base, copy a glibc TCB, or change stack-protector state.
extern "C" {
void lang_hosted_init_v1(int argc, char **argv, char **environment) noexcept;
void lang_init_tcb(const uintptr_t *initial_stack) noexcept;
// Pulled explicitly by the hosted link (-Wl,-u,lang_hosted_startup_v1 or an
// archive --whole-archive section); the constructor runs before main.
void lang_hosted_startup_v1() noexcept;
}
