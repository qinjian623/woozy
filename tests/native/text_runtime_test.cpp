#include "../../runtime/native/text.hpp"
#include "../../runtime/native/thread.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
void need(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void drop(LangText &text) {
  if (text.owner) lang_handle_release_v1(text.owner);
  text = {};
}
LangVec *bytes(const std::vector<uint8_t> &data) {
  auto *result = lang_vec_new_v1(data.size(), 1, nullptr, nullptr);
  for (auto byte : data) lang_vec_init_v1(result, &byte);
  return result;
}
LangText text(const std::vector<uint8_t> &data) {
  auto *input = bytes(data);
  LangText result;
  const auto status = lang_text_from_bytes_v1(&result, input);
  lang_vec_release_v1(input);
  need(status == 0, "valid UTF-8 rejected");
  return result;
}
void equal_bytes(const LangText &value, const std::vector<uint8_t> &expected) {
  need(value.bytes == expected.size(), "UTF-8 byte count differs");
  auto *actual = lang_text_bytes_v1(&value);
  need(actual->length == expected.size() && actual->initialized == actual->length,
       "byte conversion length differs");
  for (uint64_t i = 0; i < actual->length; ++i)
    need(*static_cast<uint8_t *>(lang_vec_at_v1(actual, i)) == expected[i],
         "UTF-8 bytes differ");
  lang_vec_release_v1(actual);
}
uint64_t frees;
void (*real_destroy)(LangHandle *) noexcept;
void observed_destroy(LangHandle *owner) noexcept {
  __atomic_fetch_add(&frees, 1, __ATOMIC_RELAXED);
  real_destroy(owner);
}
void watch(LangHandle *owner) {
  frees = 0;
  real_destroy = owner->destroy;
  owner->destroy = observed_destroy;
}
void panic(const char *cause, const auto &operation) {
  int pipe_fds[2]; need(pipe(pipe_fds) == 0, "pipe failed");
  const auto child = fork(); need(child >= 0, "fork failed");
  if (!child) {
    close(pipe_fds[0]); dup2(pipe_fds[1], 2); close(pipe_fds[1]);
    operation(); _exit(77);
  }
  close(pipe_fds[1]);
  std::string record; char buffer[256];
  for (ssize_t size; (size = read(pipe_fds[0], buffer, sizeof(buffer))) > 0;)
    record.append(buffer, size);
  close(pipe_fds[0]);
  int status; need(waitpid(child, &status, 0) == child, "waitpid failed");
  need(WIFEXITED(status) && WEXITSTATUS(status) == 101, "missing Panic exit");
  need(record.find(std::string("LANG_NATIVE_PANIC:v1:") + cause + ":") !=
           std::string::npos, "missing Panic record");
}
constexpr uint8_t mixed[] = {'A', 0xe4, 0xb8, 0xad, 0xf0, 0x9f, 0x98, 0x80,
                             'e', 0xcc, 0x81, 0, 'Z'};
constexpr uint32_t runes[] = {'A', 0x4e2d, 0x1f600, 'e', 0x301, 0, 'Z'};
void values() {
  LangText empty{};
  need(empty.bytes == 0 && empty.length == 0 && !empty.owner, "invalid empty");
  auto zero = text({}); need(!zero.data && !zero.owner && !zero.length, "empty allocated");
  LangText literal{mixed, sizeof(mixed), 7, nullptr};
  for (uint64_t i = 0; i < 7; ++i)
    need(lang_text_at_v1(&literal, i) == runes[i], "literal scalar index differs");
  auto value = text({mixed, mixed + sizeof(mixed)});
  need(value.length == 7 && value.bytes == 13 && value.owner,
       "code points were confused with bytes");
  need(value.owner->kind == LangHandleKind::text, "text owner kind differs");
  need(lang_text_compare_v1(&value, &literal) == 0, "literal/dynamic comparison differs");
  equal_bytes(value, {mixed, mixed + sizeof(mixed)});
  for (uint64_t i = 0; i < 7; ++i)
    need(lang_text_at_v1(&value, i) == runes[i], "dynamic scalar index differs");
  uint64_t cursor = value.bytes;
  for (uint64_t i = 7; i > 0; --i) {
    const uint64_t read = lang_text_cursor_read_back_v1(&value, cursor);
    need((read >> 32) == runes[i - 1], "backward cursor scalar differs");
    cursor -= static_cast<uint32_t>(read);
    need(cursor == lang_text_cursor_at_v1(&value, i - 1),
         "backward cursor moved by the wrong byte width");
  }
  LangText view;
  lang_text_slice_v1(&view, &value, 1, 6);
  LangText compacted;
  lang_text_compact_v1(&compacted, &view);
  need(compacted.owner && compacted.owner != view.owner &&
           compacted.length == view.length &&
           lang_text_compare_v1(&compacted, &view) == 0,
       "compact reused the old storage or changed the string");
  lang_text_compact_v1(&view, &view);
  need(view.owner && view.owner != value.owner &&
           lang_text_compare_v1(&view, &compacted) == 0,
       "in-place compact reused the old storage or changed the string");
  drop(compacted);
  drop(view);
  for (uint64_t start = 0; start <= 7; ++start)
    for (uint64_t end = start; end <= 7; ++end) {
      LangText slice;
      lang_text_slice_v1(&slice, &value, start, end);
      need(slice.length == end - start, "slice scalar count differs");
      for (uint64_t i = start; i < end; ++i)
        need(lang_text_at_v1(&slice, i - start) == runes[i], "slice split UTF-8");
      if (start == end) need(!slice.owner && !slice.data && !slice.bytes, "empty slice retains storage");
      drop(slice);
    }
  LangText all;
  lang_text_slice_v1(&all, &literal, 0, 7);
  need(!all.owner && all.data == literal.data, "literal slice allocated");
  drop(all);
  auto cjk = text({0xe4,0xb8,0xad,0xe6,0x96,0x87,0xed,0x95,0x9c,0xe6,0x97,0xa5});
  need(cjk.length == 4 && lang_text_at_v1(&cjk, 2) == 0xd55c, "CJK count differs");
  drop(cjk);
  auto composed = text({0xc3,0xa9});
  auto decomposed = text({'e',0xcc,0x81});
  need(composed.length == 1 && decomposed.length == 2 &&
           lang_text_compare_v1(&composed, &decomposed) > 0, "text was normalized");
  need(lang_text_compare_v1(&empty, &value) < 0 &&
           lang_text_compare_v1(&value, &empty) > 0 &&
           lang_text_compare_v1(&empty, &empty) == 0, "prefix ordering differs");
  drop(composed); drop(decomposed); drop(value);
  const uint32_t edges[] = {0,0x7f,0x80,0x7ff,0x800,0xd7ff,0xe000,0xfeff,0xffff,0x10000,0x10ffff};
  const std::vector<std::vector<uint8_t>> encodings = {
    {0},{0x7f},{0xc2,0x80},{0xdf,0xbf},{0xe0,0xa0,0x80},
    {0xed,0x9f,0xbf},{0xee,0x80,0x80},{0xef,0xbb,0xbf},{0xef,0xbf,0xbf},
    {0xf0,0x90,0x80,0x80},{0xf4,0x8f,0xbf,0xbf}};
  size_t position = 0;
  for (auto rune : edges) {
    LangText encoded;
    need(lang_text_from_rune_v1(&encoded, rune) == 0 && encoded.length == 1 &&
             lang_text_at_v1(&encoded, 0) == rune, "rune roundtrip differs");
    equal_bytes(encoded, encodings[position++]);
    for (auto other : edges) {
      LangText right;
      need(lang_text_from_rune_v1(&right, other) == 0, "comparison scalar rejected");
      const int expected = rune == other ? 0 : rune < other ? -1 : 1;
      need(lang_text_compare_v1(&encoded, &right) == expected,
           "UTF-8 ordering differs from scalar ordering");
      drop(right);
    }
    auto *data = lang_text_bytes_v1(&encoded);
    LangText decoded;
    need(lang_text_from_bytes_v1(&decoded, data) == 0 && decoded.length == 1 &&
             lang_text_compare_v1(&encoded, &decoded) == 0, "UTF-8 roundtrip differs");
    lang_vec_release_v1(data); drop(encoded); drop(decoded);
  }
}
void invalid() {
  const std::vector<std::vector<uint8_t>> bad = {
    {0x80},{0xbf},{0xc0,0x80},{0xc1,0xbf},{0xc2},{0xc2,0x20},
    {0xe0,0x80,0x80},{0xe0,0x9f,0xbf},{0xed,0xa0,0x80},{0xed,0xbf,0xbf},
    {0xe2,0x82},{0xe2,0x28,0xa1},{0xf0,0x80,0x80,0x80},{0xf0,0x8f,0xbf,0xbf},
    {0xf4,0x90,0x80,0x80},{0xf5,0x80,0x80,0x80},{0xff},{0xfe},
    {0xf0,0x9f,0x98},{0xe2,0x82,0xac,0x80},{0xf8,0x88,0x80,0x80,0x80},
    {'A',0,0xc0,0x80}};
  for (const auto &data : bad) {
    auto *input = bytes(data); LangText result{mixed,13,7,nullptr};
    need(lang_text_from_bytes_v1(&result, input) == -22 && !result.data &&
             !result.owner && !result.bytes && !result.length, "invalid UTF-8 accepted");
    lang_vec_release_v1(input);
  }
  for (uint32_t rune : {0xd800u,0xdfffu,0x110000u,0xffffffffu}) {
    LangText result{mixed,13,7,nullptr};
    need(lang_text_from_rune_v1(&result, rune) == -22 && !result.data &&
             !result.owner && !result.bytes && !result.length, "invalid scalar accepted");
  }
  LangText result;
  need(lang_text_from_bytes_v1(&result, nullptr) == -22, "null Vec accepted");
  auto *input = bytes({'a'});
  input->stride = 2; need(lang_text_from_bytes_v1(&result, input) == -22, "non-byte Vec accepted");
  input->stride = 1; input->initialized = 0;
  need(lang_text_from_bytes_v1(&result, input) == -22, "uninitialized Vec accepted");
  input->initialized = 1; lang_vec_release_v1(input);
}
void ownership() {
  auto *input = bytes({'A',0xe4,0xb8,0xad,'Z'});
  LangText value;
  need(lang_text_from_bytes_v1(&value, input) == 0, "copy from bytes failed");
  uint8_t replacement = 'B'; lang_vec_set_v1(input, 0, &replacement);
  need(lang_text_at_v1(&value, 0) == 'A', "string shares mutable Vec storage");
  auto *exported = lang_text_bytes_v1(&value);
  lang_vec_set_v1(exported, 0, &replacement);
  need(lang_text_at_v1(&value, 0) == 'A', "byte export shares immutable storage");
  lang_vec_release_v1(input); lang_vec_release_v1(exported);
  watch(value.owner);
  LangText slice; lang_text_slice_v1(&slice, &value, 1, 2);
  need(slice.owner == value.owner && value.owner->refs == 2, "slice did not retain owner");
  drop(value);
  need(!frees && lang_text_at_v1(&slice, 0) == 0x4e2d, "slice owner freed early");
  drop(slice); need(frees == 1, "shared storage was not freed once");

  value = text({'a','b'}); watch(value.owner);
  lang_text_concat_v1(&value, &value, &value);
  need(frees == 1 && value.length == 4, "concat replacement leaked old owner");
  equal_bytes(value, {'a','b','a','b'});
  lang_text_slice_v1(&value, &value, 1, 3);
  equal_bytes(value, {'b','a'});
  LangText empty{}, copied;
  auto *owner = value.owner;
  lang_text_concat_v1(&copied, &empty, &value);
  need(copied.owner == owner && owner->refs == 2, "empty concat did not retain");
  lang_text_concat_v1(&value, &empty, &value);
  need(owner->refs == 2, "empty in-place concat changed reference count");
  drop(copied); drop(value);
  lang_text_concat_v1(&empty, &empty, &empty);
  need(!empty.owner && !empty.data && !empty.length, "empty concat allocated");
  value = text({'x'}); watch(value.owner);
  lang_text_slice_v1(&value, &value, 1, 1);
  need(frees == 1 && !value.owner, "empty replacement retained old owner");
}
void failures() {
  LangText value{mixed,13,7,nullptr};
  panic("vec-index", [&]{lang_text_at_v1(&value, 7);});
  panic("vec-index", [&]{lang_text_at_v1(&value, ~uint64_t{0});});
  panic("vec-index", [&]{lang_text_cursor_read_back_v1(&value, 0);});
  panic("vec-index", [&]{lang_text_cursor_read_back_v1(&value, 2);});
  panic("vec-index", [&]{lang_text_cursor_read_back_v1(&value, value.bytes+1);});
  panic("vec-slice", [&]{LangText out;lang_text_slice_v1(&out,&value,3,2);});
  panic("vec-slice", [&]{LangText out;lang_text_slice_v1(&out,&value,0,8);});
  panic("allocation-size-overflow", [&]{
    LangText a{mixed,~uint64_t{0},1,nullptr}, out;
    lang_text_concat_v1(&out,&a,&value);
  });
  panic("allocation-size-overflow", [&]{
    LangText a{mixed,1,~uint64_t{0},nullptr}, out;
    lang_text_concat_v1(&out,&a,&value);
  });
  panic("allocation-size-overflow", []{
    LangVec input{};input.length=input.initialized=~uint64_t{0};
    input.stride=1;input.data=const_cast<uint8_t *>(mixed);LangText out;
    lang_text_from_bytes_v1(&out,&input);
  });
  panic("allocation-failed", []{
    rlimit limit{0,0}; if(setrlimit(RLIMIT_AS,&limit)) _exit(78);
    static uint8_t data[8192];LangVec input{};
    input.length=input.initialized=sizeof(data);input.stride=1;input.data=data;
    LangText out;lang_text_from_bytes_v1(&out,&input);
  });
}
void copy_context(void *pointer) {
  lang_handle_retain_v1(static_cast<LangText *>(pointer)->owner);
}
void drop_context(void *pointer) {
  lang_handle_release_v1(static_cast<LangText *>(pointer)->owner);
}
int64_t reader(void *pointer) noexcept {
  auto *text = static_cast<LangText *>(pointer);
  for (uint64_t repeat=0; repeat<1000; ++repeat) {
    for(uint64_t i=0;i<7;++i)
      if(lang_text_at_v1(text,i)!=runes[i]) return 1;
    LangText slice;lang_text_slice_v1(&slice,text,1,6);
    if(slice.length!=5 || lang_text_at_v1(&slice,1)!=0x1f600) return 2;
    lang_handle_release_v1(slice.owner);
  }
  return 0;
}
void concurrency() {
  auto value = text({mixed,mixed+sizeof(mixed)});
  LangHandle *workers[8];
  for(auto &worker:workers) {
    worker=lang_thread_spawn_v1(reader,&value,sizeof(value),copy_context,drop_context);
    need(lang_thread_error_v1(worker)==0,"reader creation failed");
  }
  drop(value);
  for(auto *worker:workers) {
    need(lang_thread_wait_v1(worker)==0 && lang_thread_result_v1(worker)==0,
         "concurrent immutable text read failed");
    lang_handle_release_v1(worker);
  }
}
} // namespace
int main() {
  try { values(); invalid(); ownership(); failures(); concurrency(); }
  catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
  std::cout<<"passed: UTF-8/codepoints, invalid encodings, slices, copies, aliases, ARC, Panic and concurrent readers\n";
}
