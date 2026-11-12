// Misuse cases for the private foreign-callback world API. Each mode is run in
// its own process and must exit 101 with the named gc-world Panic reason.

#include "gc_world.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const char *mode = argv[1];

  if (std::strcmp(mode, "callback-in-critical") == 0) {
    if (!lang_gc_world_register_v1())
      return 3;
    lang_gc_world_enter_critical_v1();
    LangGcWorldCallbackToken token{};
    lang_gc_world_callback_enter_v1(&token);
  } else if (std::strcmp(mode, "callback-unbalanced") == 0) {
    if (!lang_gc_world_register_v1())
      return 4;
    LangGcWorldCallbackToken token{};
    lang_gc_world_callback_leave_v1(&token);
  } else if (std::strcmp(mode, "callback-left-safe") == 0) {
    if (!lang_gc_world_register_v1())
      return 5;
    LangGcWorldCallbackToken token{};
    lang_gc_world_callback_enter_v1(&token);
    lang_gc_world_enter_safe_v1();
    lang_gc_world_callback_leave_v1(&token);
  } else if (std::strcmp(mode, "callback-left-critical") == 0) {
    if (!lang_gc_world_register_v1())
      return 6;
    LangGcWorldCallbackToken token{};
    lang_gc_world_callback_enter_v1(&token);
    lang_gc_world_enter_critical_v1();
    lang_gc_world_callback_leave_v1(&token);
  } else if (std::strcmp(mode, "unregister-in-callback") == 0) {
    if (!lang_gc_world_register_v1())
      return 7;
    LangGcWorldCallbackToken token{};
    lang_gc_world_callback_enter_v1(&token);
    (void)lang_gc_world_unregister_v1();
  } else if (std::strcmp(mode, "callback-in-collector") == 0) {
    if (!lang_gc_world_register_v1())
      return 8;
    if (lang_gc_world_stop_v1(0) != 0)
      return 9;
    LangGcWorldCallbackToken token{};
    lang_gc_world_callback_enter_v1(&token);
  } else if (std::strcmp(mode, "callback-token-order") == 0) {
    if (!lang_gc_world_register_v1())
      return 10;
    LangGcWorldCallbackToken outer{};
    LangGcWorldCallbackToken inner{};
    lang_gc_world_callback_enter_v1(&outer);
    lang_gc_world_callback_enter_v1(&inner);
    lang_gc_world_callback_leave_v1(&outer);
  } else if (std::strcmp(mode, "callback-token-active") == 0) {
    if (!lang_gc_world_register_v1())
      return 11;
    LangGcWorldCallbackToken token{};
    lang_gc_world_callback_enter_v1(&token);
    lang_gc_world_callback_enter_v1(&token);
  } else {
    std::fprintf(stderr, "UNKNOWN_CALLBACK_MISUSE_MODE: %s\n", mode);
    return 2;
  }
  return 12;
}
