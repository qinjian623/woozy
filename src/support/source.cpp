#include "lang/support/source.hpp"
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
                               std::string p, std::string b)
    : file(f), revision(r), id(s), path(std::move(p)), original(std::move(b)) {
  try {
    size_t i = original.starts_with("\xef\xbb\xbf") ? 3 : 0;
    Position pos{0, 0};
    offsets_.push_back(i);
    positions_[i] = pos;
    while (i < original.size()) {
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
      positions_[i] = pos;
    }
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
  if (b > e || e >= offsets_.size() || !positions_.contains(offsets_[b]) ||
      !positions_.contains(offsets_[e]))
    throw std::out_of_range("invalid span");
  return {id, offsets_[b], offsets_[e]};
}
Position SourceSnapshot::position(uint64_t b) const {
  require_valid();
  return positions_.at(b);
}
uint64_t SourceSnapshot::byte(Position p) const {
  require_valid();
  for (auto [b, v] : positions_)
    if (v == p)
      return b;
  throw std::out_of_range("invalid UTF-16 position");
}
FileId SourceManager::add_file(std::string p) {
  if (p.empty())
    throw std::invalid_argument("empty path");
  files_.push_back(std::move(p));
  return FileId(files_.size());
}
std::shared_ptr<const SourceSnapshot> SourceManager::update(FileId f,
                                                            std::string b) {
  if (!f.value || f.value > files_.size())
    throw std::out_of_range("unknown file");
  auto s = std::make_shared<const SourceSnapshot>(
      f, RevisionId(revision_ + 1), SourceSnapshotId(snapshots_.size() + 1),
      files_[f.value - 1], std::move(b));
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
