#pragma once
#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
namespace lang {
template <class Tag> struct Id {
  uint64_t value = 0;
  explicit constexpr Id(uint64_t v = 0) : value(v) {}
  auto operator<=>(const Id &) const = default;
};
struct FileIdTag;
using FileId = Id<FileIdTag>;
struct RevisionIdTag;
using RevisionId = Id<RevisionIdTag>;
struct SourceSnapshotIdTag;
using SourceSnapshotId = Id<SourceSnapshotIdTag>;
struct TypeIdTag;
using TypeId = Id<TypeIdTag>;
struct SymbolIdTag;
using SymbolId = Id<SymbolIdTag>;
struct AstNodeIdTag;
using AstNodeId = Id<AstNodeIdTag>;
struct HirNodeIdTag;
using HirNodeId = Id<HirNodeIdTag>;
struct ScopeIdTag;
using ScopeId = Id<ScopeIdTag>;
struct ModuleIdTag;
using ModuleId = Id<ModuleIdTag>;
struct ForeignBindingIdTag;
using ForeignBindingId = Id<ForeignBindingIdTag>;
struct FunctionInstanceIdTag;
using FunctionInstanceId = Id<FunctionInstanceIdTag>;
struct BlockIdTag;
using BlockId = Id<BlockIdTag>;
struct ValueIdTag;
using ValueId = Id<ValueIdTag>;
struct LocalSlotIdTag;
using LocalSlotId = Id<LocalSlotIdTag>;

struct SourceSpan {
  SourceSnapshotId snapshot;
  uint64_t begin_byte, end_byte;
};
struct Position {
  uint64_t line, character;
  auto operator<=>(const Position &) const = default;
}; // zero based UTF-16
struct Cancelled : std::exception {
  const char *what() const noexcept override { return "cancelled"; }
};
inline void check_cancelled(std::stop_token token) {
  if (token.stop_requested())
    throw Cancelled{};
}
struct SourceError : std::runtime_error {
  uint64_t byte;
  uint64_t length;
  SourceError(uint64_t b, const char *m, uint64_t n = 1)
      : runtime_error(m), byte(b), length(n) {}
};
// Checks Unicode scalar encoding only; NUL is valid metadata.
void validate_utf8(std::string_view bytes);
class SourceSnapshot {
public:
  const FileId file;
  const RevisionId revision;
  const SourceSnapshotId id;
  const std::string path, original;
  SourceSnapshot(FileId, RevisionId, SourceSnapshotId, std::string, std::string,
                 std::stop_token = {});
  bool valid() const { return !error_; }
  const std::optional<SourceError> &error() const { return error_; }
  const std::string &normalized() const;
  SourceSpan raw_span(uint64_t begin, uint64_t end) const;
  SourceSpan span(uint64_t begin, uint64_t end) const;
  Position position(uint64_t original_byte) const;
  uint64_t byte(Position) const;

private:
  struct Boundary {
    uint64_t byte;
    Position position;
  };
  std::optional<SourceError> error_;
  void require_valid() const;
  const Boundary *find_boundary(uint64_t byte) const;
  std::string text_;
  std::vector<uint64_t> offsets_;
  std::vector<Boundary> positions_;
};
class SourceManager {
  std::vector<std::string> files_;
  std::vector<std::shared_ptr<const SourceSnapshot>> snapshots_;
  uint64_t revision_ = 0;

public:
  FileId add_file(std::string path);
  std::shared_ptr<const SourceSnapshot> update(FileId, std::string bytes,
                                               std::stop_token = {});
  std::shared_ptr<const SourceSnapshot> get(SourceSnapshotId) const;
};
} // namespace lang
