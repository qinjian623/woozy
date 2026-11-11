#include "os.hpp"
#include "vec.hpp"
#include <cstddef>
#include <cstdint>

// Language buffers remain borrowed for the complete synchronous call.
// Check lengths before deriving any pointer; never expose a native address.
extern "C" int64_t lang_os_read_vec_v1(int32_t fd, LangVec *buffer,
                                       uint64_t count) noexcept {
  if (!buffer || buffer->stride != 1 || count > buffer->length ||
      count > buffer->initialized) return -22;
  if (!count) return 0;
  if (!buffer->data) return -22;
  return lang_os_read_v1(fd, buffer->data, count);
}
extern "C" int64_t lang_os_write_vec_v1(int32_t fd, LangVec *buffer,
                                        uint64_t offset, uint64_t count) noexcept {
  if (!buffer || buffer->stride != 1 || offset > buffer->length ||
      offset > buffer->initialized || count > buffer->length - offset ||
      count > buffer->initialized - offset) return -22;
  if (!count) return 0;
  if (!buffer->data) return -22;
  return lang_os_write_v1(fd, static_cast<const uint8_t *>(buffer->data) + offset, count);
}
extern "C" int64_t lang_os_write_text_v1(int32_t fd, const char *data,
                                         uint64_t length, uint64_t offset,
                                         uint64_t count) noexcept {
  if (offset > length || count > length - offset) return -22;
  if (!count) return 0;
  if (!data) return -22;
  return lang_os_write_v1(fd, data + offset, count);
}
