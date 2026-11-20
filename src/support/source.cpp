#include "lang/support/source.hpp"
#include <algorithm>
namespace lang {
namespace {
uint32_t decode(std::string_view s, size_t i, size_t &n) {
  auto c = static_cast<unsigned char>(s[i]);
  n = 1;
  if (c < 128)
    return c;
  uint32_t v;
  uint32_t min;
  if (c >= 0xc2 && c <= 0xdf) {
    n = 2;
    v = c & 31;
    min = 128;
  } else if (c >= 0xe0 && c <= 0xef) {
    n = 3;
    v = c & 15;
    min = 2048;
  } else if (c >= 0xf0 && c <= 0xf4) {
    n = 4;
    v = c & 7;
    min = 65536;
  } else
    throw SourceError(i, "invalid UTF-8");
  if (n > s.size() - i)
    throw SourceError(i, "truncated UTF-8", s.size() - i);
  for (size_t j = 1; j < n; ++j) {
    auto b = static_cast<unsigned char>(s[i + j]);
    if ((b & 192) != 128)
      throw SourceError(i, "invalid continuation", j);
    v = (v << 6) | (b & 63);
  }
  if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
    throw SourceError(i, "invalid scalar", n);
  return v;
}
} // namespace
void validate_utf8(std::string_view bytes) {
  for (size_t i = 0; i < bytes.size();) {
    size_t n;
    decode(bytes, i, n);
    i += n;
  }
}
SourceSnapshot::SourceSnapshot(FileId f, RevisionId r, SourceSnapshotId s,
                               std::string p, std::string b,
                               std::stop_token token)
    : file(f), revision(r), id(s), path(std::move(p)), original(std::move(b)) {
  check_cancelled(token);
  try {
    size_t i = original.starts_with("\xef\xbb\xbf") ? 3 : 0;
    Position pos{0, 0};
    offsets_.push_back(i);
    positions_.push_back({i, pos});
    while (i < original.size()) {
      check_cancelled(token);
      size_t n;
      auto cp = decode(original, i, n);
      if (cp == 0)
        throw SourceError(i, "raw NUL");
      if (cp == 13) {
        if (i + 1 >= original.size() || original[i + 1] != '\n')
          throw SourceError(i, "standalone CR");
        n = 2;
        cp = 10;
        text_ += '\n';
        offsets_.push_back(i + 2);
      } else {
        text_.append(original, i, n);
        for (size_t j = 1; j <= n; ++j)
          offsets_.push_back(i + j);
      }
      i += n;
      if (cp == 10) {
        ++pos.line;
        pos.character = 0;
      } else
        pos.character += cp > 65535 ? 2 : 1;
      positions_.push_back({i, pos});
    }
    check_cancelled(token);
  } catch (const SourceError &e) {
    error_ = e;
    text_.clear();
    offsets_.clear();
    positions_.clear();
  }
}
void SourceSnapshot::require_valid() const {
  if (error_)
    throw *error_;
}
const SourceSnapshot::Boundary *SourceSnapshot::find_boundary(uint64_t byte) const {
  auto found = std::ranges::lower_bound(positions_, byte, {}, &Boundary::byte);
  if (found != positions_.end() && found->byte == byte)
    return &*found;
  return nullptr;
}
const std::string &SourceSnapshot::normalized() const {
  require_valid();
  return text_;
}
SourceSpan SourceSnapshot::raw_span(uint64_t b, uint64_t e) const {
  if (b > e || e > original.size())
    throw std::out_of_range("invalid raw span");
  return {id, b, e};
}
SourceSpan SourceSnapshot::span(uint64_t b, uint64_t e) const {
  require_valid();
  if (b > e || e >= offsets_.size() || !find_boundary(offsets_[b]) ||
      !find_boundary(offsets_[e]))
    throw std::out_of_range("invalid span");
  return {id, offsets_[b], offsets_[e]};
}
Position SourceSnapshot::position(uint64_t b) const {
  require_valid();
  if (auto found = find_boundary(b))
    return found->position;
  throw std::out_of_range("invalid original byte position");
}
uint64_t SourceSnapshot::byte(Position p) const {
  require_valid();
  // Normalization emits a boundary only after consuming a scalar or newline.
  auto found = std::ranges::lower_bound(positions_, p, {}, &Boundary::position);
  if (found != positions_.end() && found->position == p)
    return found->byte;
  throw std::out_of_range("invalid UTF-16 position");
}
FileId SourceManager::add_file(std::string p) {
  if (p.empty())
    throw std::invalid_argument("empty path");
  files_.push_back(std::move(p));
  return FileId(files_.size());
}
std::shared_ptr<const SourceSnapshot>
SourceManager::update(FileId f, std::string b, std::stop_token token) {
  check_cancelled(token);
  if (!f.value || f.value > files_.size())
    throw std::out_of_range("unknown file");
  auto s = std::make_shared<const SourceSnapshot>(
      f, RevisionId(revision_ + 1), SourceSnapshotId(snapshots_.size() + 1),
      files_[f.value - 1], std::move(b), token);
  check_cancelled(token);
  ++revision_;
  snapshots_.push_back(s);
  return s;
}
std::shared_ptr<const SourceSnapshot>
SourceManager::get(SourceSnapshotId s) const {
  if (!s.value || s.value > snapshots_.size())
    throw std::out_of_range("unknown snapshot");
  return snapshots_[s.value - 1];
}
} // namespace lang
