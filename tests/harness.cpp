// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "harness.hpp"

#include <cstring>

namespace qostest {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

int& failure_count() {
  static int failures = 0;
  return failures;
}

int& check_count() {
  static int checks = 0;
  return checks;
}

bool truthy(bool value) noexcept { return value; }

void record_failure(const char* file, int line, const std::string& message) {
  ++failure_count();
  std::printf("  FAIL %s:%d\n       %s\n", file, line, message.c_str());
  std::fflush(stdout);
}

int run_all(int argc, char** argv) {
  const char* filter = (argc > 1) ? argv[1] : nullptr;
  int executed = 0;
  for (const TestCase& test : registry()) {
    if (filter != nullptr && std::strstr(test.name, filter) == nullptr) {
      continue;
    }
    const int before = failure_count();
    std::printf("[ RUN  ] %s\n", test.name);
    std::fflush(stdout);
    test.function();
    const bool ok = failure_count() == before;
    std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", test.name);
    std::fflush(stdout);
    ++executed;
  }
  std::printf("\n%d test(s) executed, %d check(s), %d failure(s)\n", executed, check_count(),
              failure_count());
  if (executed == 0) {
    std::printf("no test matched the filter\n");
    return 2;
  }
  return failure_count() == 0 ? 0 : 1;
}

}  // namespace qostest

int main(int argc, char** argv) { return ::qostest::run_all(argc, argv); }
