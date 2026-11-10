// Test-only C++ allocation trace for the binarytrees fixture.
//
// The reference source is compiled with -Dmain=tree_program_main, then linked
// with this file.  Global operator new/delete record the exact Node pointer and
// allocation size; the program allocates only Node objects.  The observer main
// prints the final count/live/peak line after tree_program_main has returned,
// so no host static-destructor order is assumed.
//
// This is not a formal performance program.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

std::FILE *trace = nullptr;
std::uint64_t allocations = 0;
std::uint64_t frees = 0;
std::uint64_t live = 0;
std::uint64_t peak = 0;

void ensure_trace() {
    if (trace != nullptr) {
        return;
    }
    const char *path = std::getenv("BT_TRACE_PATH");
    if (path == nullptr || path[0] == '\0') {
        std::fputs("cpp_observer: BT_TRACE_PATH is not set\n", stderr);
        std::_Exit(97);
    }
    trace = std::fopen(path, "w");
    if (trace == nullptr) {
        std::fputs("cpp_observer: cannot open BT_TRACE_PATH\n", stderr);
        std::_Exit(98);
    }
    std::setvbuf(trace, nullptr, _IOFBF, 1u << 20);
}

void record_alloc(void *pointer, std::size_t size) {
    ++allocations;
    ++live;
    if (live > peak) {
        peak = live;
    }
    std::fprintf(trace, "ALLOC:%p:%zu\n", pointer, size);
}

void record_free(void *pointer) {
    if (live == 0) {
        std::fputs("cpp_observer: delete without live allocation\n", stderr);
        std::_Exit(99);
    }
    ++frees;
    --live;
    std::fprintf(trace, "FREE:%p\n", pointer);
    std::fflush(trace);
}

}  // namespace

void *operator new(std::size_t size) {
    if (size == 0) {
        size = 1;
    }
    void *pointer = std::malloc(size);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    ensure_trace();
    record_alloc(pointer, size);
    return pointer;
}

void *operator new[](std::size_t size) {
    return operator new(size);
}

void operator delete(void *pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }
    ensure_trace();
    record_free(pointer);
    std::free(pointer);
}

void operator delete(void *pointer, std::size_t) noexcept {
    operator delete(pointer);
}

void operator delete[](void *pointer) noexcept {
    operator delete(pointer);
}

void operator delete[](void *pointer, std::size_t) noexcept {
    operator delete(pointer);
}

int tree_program_main();

int main() {
    ensure_trace();
    const int result = tree_program_main();
    std::fprintf(trace, "TOTAL:%llu:%llu:%llu:%llu\n",
                 static_cast<unsigned long long>(allocations),
                 static_cast<unsigned long long>(frees),
                 static_cast<unsigned long long>(live),
                 static_cast<unsigned long long>(peak));
    std::fflush(trace);
    return result;
}
