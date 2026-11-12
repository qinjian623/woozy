// Test-only allocator fork hooks for the hosted sanitizer build.
//
// The sanitizer allocator replaces runtime/native/alloc.cpp, so it must also
// supply the three allocator fork entry points used by process.cpp. They are
// intentionally no-ops: the sanitizer build uses malloc/free, which has no
// inherited runtime ticket state.

extern "C" void lang_alloc_fork_prepare_v1() noexcept {}
extern "C" void lang_alloc_fork_parent_v1() noexcept {}
extern "C" void lang_alloc_fork_child_v1() noexcept {}
