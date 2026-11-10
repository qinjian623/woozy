// Test-only Native trace for the binarytrees fixture.
//
// The Source program object is linked with this file and the project runtime
// using --wrap=lang_alloc_v1/--wrap=lang_free_v1.  The strong
// lang_vec_observe_v1 observer is the runtime's existing weak observation ABI
// from runtime/native/panic.cpp; the LangVec fields are read through the real
// runtime/native/vec.hpp header.  The trace records the exact Vec identity,
// length, stride and refs, plus every real allocator allocation and free.
//
// This is not a formal CLI program and is never used for timing.  It depends on
// the host libc for stdio and is recorded separately from the static Native ELF.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "vec.hpp"

namespace {

std::FILE *trace = nullptr;

void ensure_trace() {
    if (trace != nullptr) {
        return;
    }
    const char *path = std::getenv("BT_TRACE_PATH");
    if (path == nullptr || path[0] == '\0') {
        std::fputs("native_observer: BT_TRACE_PATH is not set\n", stderr);
        std::_Exit(97);
    }
    trace = std::fopen(path, "w");
    if (trace == nullptr) {
        std::fputs("native_observer: cannot open BT_TRACE_PATH\n", stderr);
        std::_Exit(98);
    }
    std::setvbuf(trace, nullptr, _IOFBF, 1u << 20);
}

}  // namespace

extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t) noexcept;
extern "C" void __real_lang_free_v1(void *) noexcept;

extern "C" void *__wrap_lang_alloc_v1(uint64_t size,
                                      uint64_t align) noexcept {
    void *pointer = __real_lang_alloc_v1(size, align);
    if (pointer != nullptr) {
        ensure_trace();
        std::fprintf(trace, "ALLOC:%p:%llu:%llu\n", pointer,
                     static_cast<unsigned long long>(size),
                     static_cast<unsigned long long>(align));
    }
    return pointer;
}

extern "C" void __wrap_lang_free_v1(void *pointer) noexcept {
    if (pointer != nullptr) {
        ensure_trace();
        std::fprintf(trace, "FREE:%p\n", pointer);
        // Flush at each real free so the trace is complete even at a raw
        // runtime exit after the final release path.
        std::fflush(trace);
    }
    __real_lang_free_v1(pointer);
}

extern "C" void lang_vec_observe_v1(uint32_t event, const void *object,
                                    uint64_t detail) {
    ensure_trace();
    const auto *value = static_cast<const LangVec *>(object);
    std::fprintf(trace, "VEC:%u:%p:%llu:%llu:%llu:%llu\n", event, object,
                 static_cast<unsigned long long>(detail),
                 static_cast<unsigned long long>(value->length),
                 static_cast<unsigned long long>(value->stride),
                 static_cast<unsigned long long>(value->refs));
}
