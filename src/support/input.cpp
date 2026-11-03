#include "lang/support/input.hpp"
#include <array>
#include <fstream>
namespace lang {
PhaseResult<std::shared_ptr<const SourceSnapshot>>
read_source(const std::filesystem::path &path, SourceManager &manager,
            std::stop_token token) {
  PhaseResult<std::shared_ptr<const SourceSnapshot>> result;
  try {
    check_cancelled(token);
    std::ifstream file(path, std::ios::binary);
    std::string bytes;
    std::array<char, 65536> buffer;
    if (!file.is_open())
      throw std::ios_base::failure("cannot open input");
    for (;;) {
      check_cancelled(token);
      file.read(buffer.data(), buffer.size());
      bytes.append(buffer.data(), static_cast<size_t>(file.gcount()));
      check_cancelled(token);
      if (file.bad() || (file.fail() && !file.eof()))
        throw std::ios_base::failure("cannot read input");
      if (file.eof())
        break;
    }
    check_cancelled(token);
    auto snapshot = manager.update(manager.add_file(path.string()),
                                   std::move(bytes), token);
    if (!snapshot->valid()) {
      result.failure = FailureKind::source;
      const auto &e = *snapshot->error();
      result.diagnostics.push_back(
          {"E_INVALID_SOURCE",
           "lex",
           "error",
           e.what(),
           snapshot->raw_span(e.byte, e.byte + e.length),
           {},
           {},
           {},
           {},
           {}});
    } else {
      result.state = PhaseState::complete;
      result.value = snapshot;
    }
  } catch (const Cancelled &) {
    result.state = PhaseState::cancelled;
  } catch (const std::ios_base::failure &e) {
    result.failure = FailureKind::environment;
    result.diagnostics.push_back({"E_INPUT_IO",
                                  "toolchain",
                                  "error",
                                  path.string() + ": " + e.what(),
                                  {},
                                  {},
                                  {},
                                  {},
                                  {},
                                  {}});
  }
  return result;
}
} // namespace lang
