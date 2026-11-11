#include "network.hpp"
#include "text.hpp"
#include <cstddef>

namespace {
bool byte_range(const LangVec *value, uint64_t offset, uint64_t count) noexcept {
  return value && value->stride == 1 && value->initialized == value->length &&
      !value->copy && !value->drop && offset <= value->length &&
      count <= value->length-offset && (!count || value->data);
}
void *bytes(LangVec *value, uint64_t offset = 0) noexcept {
  return value->data ? static_cast<uint8_t *>(value->data)+offset : nullptr;
}
struct Accepted { int32_t descriptor; uint32_t padding; LangNetAddress address; };
struct Received { uint64_t count; LangNetAddress address; };
static_assert(sizeof(Accepted) == 32 && offsetof(Accepted,address) == 8);
static_assert(sizeof(Received) == 32 && offsetof(Received,address) == 8);
}
extern "C" int64_t lang_net_send_vec_v1(int32_t fd, LangVec *data,
    uint64_t offset, uint64_t count, int32_t flags) noexcept {
  if (!byte_range(data,offset,count)) return -22;
  return lang_net_send_v1(fd,bytes(data,offset),count,flags);
}
extern "C" int64_t lang_net_recv_vec_v1(int32_t fd, LangVec *data,
    uint64_t count, int32_t flags) noexcept {
  if (!byte_range(data,0,count)) return -22;
  return lang_net_recv_v1(fd,bytes(data),count,flags);
}
extern "C" int64_t lang_net_sendto_vec_v1(int32_t fd, LangVec *data,
    uint64_t count, int32_t flags, const LangNetAddress *address) noexcept {
  if (!byte_range(data,0,count)) return -22;
  return lang_net_sendto_v1(fd,bytes(data),count,flags,address);
}
extern "C" int64_t lang_net_recvfrom_vec_v1(int32_t fd, LangVec *data,
    uint64_t count, int32_t flags, Received *out) noexcept {
  if (!out || !byte_range(data,0,count)) return -22;
  LangNetAddress address;
  const auto result = lang_net_recvfrom_v1(fd,bytes(data),count,flags,&address);
  if (result < 0) return result;
  *out = {static_cast<uint64_t>(result),address};
  return 0;
}
extern "C" int64_t lang_net_accept_record_v1(int32_t fd, uint32_t flags,
    Accepted *out) noexcept {
  if (!out) return -22;
  LangNetAddress address;
  const auto result = lang_net_accept_v1(fd,&address,flags);
  if (result < 0) return result;
  *out = {static_cast<int32_t>(result),0,address};
  return 0;
}
extern "C" int64_t lang_net_poll_vec_v1(LangVec *rows, uint64_t count,
    int32_t timeout) noexcept {
  if (!rows || rows->stride != sizeof(LangPollFd) || rows->copy || rows->drop ||
      count > rows->length || count > rows->initialized || (count && !rows->data)) return -22;
  return lang_net_poll_v1(static_cast<LangPollFd *>(rows->data),count,timeout);
}
extern "C" int64_t lang_net_setsockopt_vec_v1(int32_t fd, int32_t level,
    int32_t option, LangVec *data, uint64_t count) noexcept {
  if (!byte_range(data,0,count)) return -22;
  return lang_net_setsockopt_bytes_v1(fd,level,option,bytes(data),count);
}
extern "C" int64_t lang_net_getsockopt_vec_v1(int32_t fd, int32_t level,
    int32_t option, LangVec *data, uint64_t count) noexcept {
  if (!byte_range(data,0,count)) return -22;
  auto length = count;
  const auto result = lang_net_getsockopt_bytes_v1(fd,level,option,bytes(data),&length);
  return result < 0 ? result : static_cast<int64_t>(length);
}
extern "C" int64_t lang_net_format_address_v1(const LangNetAddress *address,
    LangText *out) noexcept {
  if (!out) return -22;
  char buffer[64];
  const auto count = lang_net_inet_ntop_v1(address,buffer,sizeof(buffer));
  if (count < 0) return count;
  return lang_text_from_utf8_v1(out,reinterpret_cast<const uint8_t *>(buffer),count);
}
