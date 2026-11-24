#pragma once
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace ps3dmvp {
// An exception escaping a codec worker never reaches the application's main
// catch block. Preserve its message before the runtime terminates the process.
inline void reportUnhandledException(std::FILE* out) noexcept {
  std::fputs("FATAL: unhandled codec exception: ", out);
  const auto failure = std::current_exception();
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception& e) { std::fprintf(out, "%s\n", e.what()); }
    catch (...) { std::fputs("non-standard exception\n", out); }
  } else {
    std::fputs("terminate called without an active exception\n", out);
  }
  std::fflush(out);
}
inline void installFailureDiagnostics() noexcept {
  std::set_terminate([]() noexcept {
    reportUnhandledException(stderr);
    std::abort();
  });
}
}
