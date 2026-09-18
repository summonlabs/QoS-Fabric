// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A deliberately small test harness: no external dependency, no timeout, no
// hidden retry. A failing check records the expression, the file and the line.
// Tests are always run to completion -- a hanging test is a defect to
// diagnose, never something to kill.

#ifndef QOSFABRIC_TEST_HARNESS_HPP
#define QOSFABRIC_TEST_HARNESS_HPP

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace qostest {

using TestFunction = void (*)();

struct TestCase {
  const char* name;
  TestFunction function;
};

std::vector<TestCase>& registry();
int& failure_count();
int& check_count();

struct Registrar {
  Registrar(const char* name, TestFunction function) {
    registry().push_back(TestCase{name, function});
  }
};

void record_failure(const char* file, int line, const std::string& message);

// Evaluated in a separate translation unit so that a condition the compiler
// can fold to a constant never turns into a conditional-expression-is-constant
// warning inside a macro expansion.
bool truthy(bool value) noexcept;

// Evaluated in a separate translation unit so that a condition the compiler
// can fold to a constant never turns into a "conditional expression is
// constant" warning inside a macro expansion.
bool truthy(bool value) noexcept;

// Renders a value for a failure message. One template rather than an overload
// set, so an integer comparison can never become ambiguous; types the harness
// cannot print are reported as unprintable rather than silently mis-typed.
template <class T>
std::string describe(const T& value) {
  if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
    return value;
  } else if constexpr (std::is_convertible_v<T, const char*>) {
    const char* text = value;
    return text == nullptr ? std::string("(null)") : std::string(text);
  } else if constexpr (std::is_enum_v<std::decay_t<T>>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_integral_v<std::decay_t<T>>) {
    return std::to_string(value);
  } else {
    return std::string("<unprintable>");
  }
}

int run_all(int argc, char** argv);

}  // namespace qostest

#define QOS_TEST(name)                                                                static void name();                                                                 static ::qostest::Registrar qos_registrar_##name(#name, &name);                      static void name()

#define QOS_CHECK(condition)                                                          do {                                                                                  ++::qostest::check_count();                                                          if (!::qostest::truthy(static_cast<bool>(condition))) {                                                                   ::qostest::record_failure(__FILE__, __LINE__, "CHECK failed: " #condition);        }                                                                                 } while (false)

#define QOS_CHECK_EQ(actual, expected)                                                do {                                                                                  ++::qostest::check_count();                                                          const auto qos_actual = (actual);                                                    const auto qos_expected = (expected);                                                if (!::qostest::truthy(static_cast<bool>(qos_actual == qos_expected))) {                                                   ::qostest::record_failure(__FILE__, __LINE__,                                                                  std::string("CHECK_EQ failed: " #actual " == "                                                   #expected " (actual=") +                                                     ::qostest::describe(qos_actual) + " expected=" +                                     ::qostest::describe(qos_expected) + ")");            }                                                                                 } while (false)

#define QOS_REQUIRE(condition)                                                        do {                                                                                  ++::qostest::check_count();                                                          if (!::qostest::truthy(static_cast<bool>(condition))) {                                                                   ::qostest::record_failure(__FILE__, __LINE__, "REQUIRE failed: " #condition);        return;                                                                           }                                                                                 } while (false)

#endif  // QOSFABRIC_TEST_HARNESS_HPP
