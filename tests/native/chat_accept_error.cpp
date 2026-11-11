#include <cstdint>
#include "../../runtime/native/linux.hpp"
#ifndef CHAT_ACCEPT_ERROR
#define CHAT_ACCEPT_ERROR 103
#endif
#define CHAT_STRING_INNER(value) #value
#define CHAT_STRING(value) CHAT_STRING_INNER(value)
extern "C" int64_t __real_lang_net_accept_record_v1(int32_t, uint32_t, void *) noexcept;
extern "C" int64_t __wrap_lang_net_accept_record_v1(int32_t fd, uint32_t flags, void *out) noexcept {
    static unsigned calls = 0;
    // One existing connection, then EAGAIN; fail the next connection attempt.
    if (++calls == 3) {
        static constexpr char marker[] = "CHAT_ACCEPT_ERROR:" CHAT_STRING(CHAT_ACCEPT_ERROR) "\n";
        lang::native::linux_x86_64::syscall6(1, 2, reinterpret_cast<long>(marker), sizeof(marker)-1, 0, 0, 0);
        return -CHAT_ACCEPT_ERROR;
    }
    return __real_lang_net_accept_record_v1(fd, flags, out);
}
