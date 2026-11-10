#include "lang/support/input.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
using namespace lang;
void check(bool b) {
  if (!b)
    throw std::runtime_error("check failed");
}
int main() {
  PhaseResult<int> r;
  r.state = PhaseState::complete;
  check(!r.success());
  check(r.exit_code() == ExitCode::internal_error);
  check(r.status() == Status::internal_error);
  r.value = 0;
  check(r.success());
  r.state = PhaseState::recovered;
  check(!r.success());
  r.state = PhaseState::complete;
  r.diagnostics.push_back(
      {"E_TEST", "lex", "error", "error", {}, {}, {}, {}, {}, {}});
  check(!r.success());
  for (auto [kind, code] : {std::pair{FailureKind::source, 1},
                            {FailureKind::unsupported, 1},
                            {FailureKind::environment, 2},
                            {FailureKind::internal, 3}}) {
    r.failure = kind;
    check(static_cast<int>(r.exit_code()) == code);
  }
  SourceManager m;
  auto f = m.add_file("old.wz");
  auto old = m.update(f, "old");
  std::stop_source stop;
  stop.request_stop();
  auto cancelled = read_source("/missing", m, stop.get_token());
  check(cancelled.exit_code() == ExitCode::cancelled);
  check(cancelled.diagnostics.empty());
  std::cout << diagnostic_json(m, "test", "1", cancelled.status(),
                               cancelled.diagnostics)
            << '\n';
  auto directory =
      std::filesystem::temp_directory_path() /
      ("lang-phase-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  check(std::filesystem::create_directory(directory));
  auto missing = read_source(directory / "missing.wz", m);
  check(missing.exit_code() == ExitCode::environment_error);
  check(missing.diagnostics.at(0).code == "E_INPUT_IO");
  std::cout << diagnostic_json(m, "test", "1", missing.status(),
                               missing.diagnostics)
            << '\n';
  auto path = directory / "input.wz";
  for (auto bytes :
       {std::string("a\x80"), std::string("a\0", 2), std::string("a\r")}) {
    {
      std::ofstream out(path, std::ios::binary);
      out.write(bytes.data(), bytes.size());
    }
    auto bad = read_source(path, m);
    check(bad.exit_code() == ExitCode::source_error);
    const auto &d = bad.diagnostics.at(0);
    check(d.code == "E_INVALID_SOURCE" && d.phase == "lex");
    check(d.primary->begin_byte == 1 && d.primary->end_byte == 2);
    check(m.get(d.primary->snapshot)->original == bytes);
    std::cout << diagnostic_json(m, "test", "1", bad.status(), bad.diagnostics)
              << '\n';
  }
  const std::string bytes = "\xef\xbb\xbf中\r\n";
  {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
  }
  auto good = read_source(path, m);
  check(good.success() && (*good.value)->original == bytes);
  auto previous = (*good.value)->id.value;
  std::atomic<bool> entered = false;
  std::stop_source during;
  std::string large(4000000, 'x');
  std::jthread worker([&] {
    entered = true;
    try {
      m.update(f, std::move(large), during.get_token());
      check(false);
    } catch (const Cancelled &) {
    }
  });
  while (!entered)
    std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  during.request_stop();
  worker.join();
  check(m.get(old->id) == old && old->normalized() == "old");
  check(m.update(f, "next")->id.value == previous + 1);
  std::filesystem::remove(path);
  std::filesystem::remove(directory);
}
