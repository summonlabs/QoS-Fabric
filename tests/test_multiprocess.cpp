// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Multi-process validation.
//
// REAL: real operating-system processes, a real TCP loopback socket, a real
// framed protocol, a real abrupt process termination. NOT claimed: multi-node,
// multi-switch, RDMA, NVLink, optical or NIC validation. Everything here runs
// on one host over the loopback interface.

#include "harness.hpp"
#include "tempdir.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "qosfabric/qosfabric.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

using namespace qosfabric;

namespace {

#ifndef QOSFABRIC_CLI_PATH
#define QOSFABRIC_CLI_PATH "qosfabric_cli"
#endif
#ifndef QOSFABRIC_PUBLISHER_PATH
#define QOSFABRIC_PUBLISHER_PATH "qosfabric_publisher"
#endif

std::string quote(const std::string& value) {
  std::string out = "\"";
  for (const char c : value) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

class Child {
 public:
  Child() = default;
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  Child(Child&& other) noexcept : valid_(other.valid_), process_(other.process_) {
    other.valid_ = false;
    other.process_ = nullptr;
  }
  Child& operator=(Child&& other) noexcept {
    if (this != &other) {
      close_handle();
      valid_ = other.valid_;
      process_ = other.process_;
      other.valid_ = false;
      other.process_ = nullptr;
    }
    return *this;
  }

  static Child spawn(const std::string& executable, const std::vector<std::string>& arguments,
                     const std::string& stdout_path) {
    Child child;
#ifdef _WIN32
    std::string command = quote(executable);
    for (const std::string& argument : arguments) {
      command += " ";
      command += quote(argument);
    }
    std::vector<wchar_t> wide_command(command.begin(), command.end());
    wide_command.push_back(L'\0');

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    if (!stdout_path.empty()) {
      const std::wstring wide_path(stdout_path.begin(), stdout_path.end());
      output = ::CreateFileW(wide_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (output != INVALID_HANDLE_VALUE) {
      startup.dwFlags |= STARTF_USESTDHANDLES;
      startup.hStdOutput = output;
      startup.hStdError = output;
      startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION info{};
    const BOOL created =
        ::CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                         &startup, &info);
    if (output != INVALID_HANDLE_VALUE) {
      ::CloseHandle(output);
    }
    if (created == 0) {
      child.valid_ = false;
      return child;
    }
    ::CloseHandle(info.hThread);
    child.process_ = info.hProcess;
    child.valid_ = true;
#else
    (void)executable;
    (void)arguments;
    (void)stdout_path;
    child.valid_ = false;
#endif
    return child;
  }

  ~Child() { close_handle(); }

  [[nodiscard]] bool valid() const noexcept { return valid_; }

  [[nodiscard]] bool running() const {
#ifdef _WIN32
    if (process_ == nullptr) {
      return false;
    }
    return ::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
    return false;
#endif
  }

  // Waits for natural completion and returns the exit code.
  int wait() {
#ifdef _WIN32
    if (process_ == nullptr) {
      return -1;
    }
    ::WaitForSingleObject(process_, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(process_, &code);
    return static_cast<int>(code);
#else
    return -1;
#endif
  }

  // Abrupt termination: no clean shutdown, no final flush, no cooperation.
  void terminate() {
#ifdef _WIN32
    if (process_ != nullptr) {
      ::TerminateProcess(process_, 9);
      ::WaitForSingleObject(process_, INFINITE);
    }
#endif
  }

 private:
  void close_handle() {
#ifdef _WIN32
    if (process_ != nullptr) {
      ::CloseHandle(process_);
      process_ = nullptr;
    }
    valid_ = false;
#endif
  }

  bool valid_{false};
#ifdef _WIN32
  HANDLE process_{nullptr};
#endif
};

std::string read_text(const std::string& path) {
  const auto bytes = read_file(path, 1u << 20);
  if (!bytes) {
    return {};
  }
  return std::string(bytes->begin(), bytes->end());
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// Waits for a file to appear. This is a readiness wait with a deadline, not a
// test timeout: if the deadline passes the test reports the failure and
// continues rather than being killed.
bool wait_for_file(const std::string& path, std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!read_text(path).empty()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return !read_text(path).empty();
}

std::uint16_t parse_port(const std::string& text) {
  std::uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      break;
    }
    value = value * 10u + static_cast<std::uint32_t>(c - '0');
    if (value > 65535u) {
      break;
    }
  }
  return static_cast<std::uint16_t>(value);
}

std::vector<std::string> publisher_arguments(const std::string& port, const std::string& boot_file,
                                             const std::string& resource,
                                             const std::string& extra_flag = {},
                                             const std::string& extra_value = {}) {
  std::vector<std::string> arguments = {
      "--host",       "127.0.0.1",     "--port",     port,
      "--publisher-id", "bus-1",       "--boot-file", boot_file,
      "--resource",   resource,        "--ttl-ms",   "600000",
      "--state",      "available",     "--rate",     "5000000",
      "--latency",    "500",           "--priority", "4",
      "--domain",     "domain-1",      "--quiet"};
  if (!extra_flag.empty()) {
    arguments.push_back(extra_flag);
    arguments.push_back(extra_value);
  }
  return arguments;
}

}  // namespace

QOS_TEST(real_processes_publish_over_real_framed_transport) {
  qostest::TempDir directory("multiprocess");
  const std::string store_path = directory.child("store");
  const std::string port_file = directory.child("port.txt");
  const std::string boot_file = directory.child("publisher.boot");

  {
    auto registry = Registry::Open([&] {
      RegistryOptions options;
      options.directory = store_path;
      return options;
    }());
    QOS_REQUIRE(registry.has_value());
    QOS_REQUIRE((*registry)->Close().has_value());
  }

  Child server = Child::spawn(QOSFABRIC_CLI_PATH,
                              {"serve", "--store", store_path, "--port-file", port_file},
                              directory.child("server.log"));
  QOS_REQUIRE(server.valid());
  QOS_REQUIRE(wait_for_file(port_file, std::chrono::milliseconds(30000)));
  const std::string port_text = read_text(port_file);
  const std::uint16_t port = parse_port(port_text);
  QOS_CHECK(port != 0);

  // First real publisher process.
  {
    Child publisher = Child::spawn(QOSFABRIC_PUBLISHER_PATH,
                                   publisher_arguments(std::to_string(port), boot_file, "r1"),
                                   directory.child("pub1.log"));
    QOS_REQUIRE(publisher.valid());
    const int code = publisher.wait();
    const std::string log = read_text(directory.child("pub1.log"));
    QOS_CHECK_EQ(code, 0);
    QOS_CHECK(contains(log, "HELLO bus-1 incarnation=1"));
    QOS_CHECK(contains(log, "status=ok"));
    QOS_CHECK(contains(log, "PUBLISH r1 generation=1"));
  }

  // A second process restart takes the next incarnation and is accepted.
  {
    Child publisher = Child::spawn(QOSFABRIC_PUBLISHER_PATH,
                                   publisher_arguments(std::to_string(port), boot_file, "r1"),
                                   directory.child("pub2.log"));
    const int code = publisher.wait();
    const std::string log = read_text(directory.child("pub2.log"));
    QOS_CHECK_EQ(code, 0);
    QOS_CHECK(contains(log, "incarnation=2"));
    QOS_CHECK(contains(log, "PUBLISH r1 generation=2"));
  }

  // Rewinding the durable boot counter must not resurrect an old authority:
  // the registry fences the reused incarnation across a real process boundary.
  {
    const std::string rewind = "1\n";
    const std::vector<std::uint8_t> bytes(rewind.begin(), rewind.end());
    QOS_REQUIRE(write_file_atomic(boot_file, bytes).has_value());
    Child publisher = Child::spawn(QOSFABRIC_PUBLISHER_PATH,
                                   publisher_arguments(std::to_string(port), boot_file, "r1"),
                                   directory.child("pub3.log"));
    const int code = publisher.wait();
    const std::string log = read_text(directory.child("pub3.log"));
    QOS_CHECK_EQ(code, 2);
    QOS_CHECK(contains(log, "incarnation=2"));
    QOS_CHECK(contains(log, "status=fenced"));
  }

  // The boot counter now continues from the rewound value.
  {
    Child publisher = Child::spawn(QOSFABRIC_PUBLISHER_PATH,
                                   publisher_arguments(std::to_string(port), boot_file, "r1"),
                                   directory.child("pub4.log"));
    const int code = publisher.wait();
    const std::string log = read_text(directory.child("pub4.log"));
    // The fenced attempt still advanced the durable boot counter, so the next
    // incarnation is strictly newer and is accepted.
    QOS_CHECK_EQ(code, 0);
    QOS_CHECK(contains(log, "incarnation=3"));
    QOS_CHECK(contains(log, "PUBLISH r1 generation=3"));
  }

  // Abrupt termination of the registry process: no clean shutdown marker.
  server.terminate();

  // A publisher that arrives after the authority died must fail, not silently
  // succeed.
  {
    Child publisher = Child::spawn(QOSFABRIC_PUBLISHER_PATH,
                                   publisher_arguments(std::to_string(port), boot_file, "r1"),
                                   directory.child("pub5.log"));
    const int code = publisher.wait();
    QOS_CHECK(code != 0);
  }

  // Recovery of the same durable store in a brand new process.
  {
    Child status = Child::spawn(QOSFABRIC_CLI_PATH, {"status", "--store", store_path},
                                directory.child("status.log"));
    const int code = status.wait();
    const std::string log = read_text(directory.child("status.log"));
    QOS_CHECK_EQ(code, 0);
    QOS_CHECK(contains(log, "capabilities=3"));
    QOS_CHECK(contains(log, "leases=0"));
    QOS_CHECK(contains(log, "clean=0"));
  }

  // A purely observational open claims no authority, so it does not move the
  // epoch; it does record a clean shutdown marker, which the next open sees.
  {
    Child status = Child::spawn(QOSFABRIC_CLI_PATH, {"status", "--store", store_path},
                                directory.child("status2.log"));
    QOS_CHECK_EQ(status.wait(), 0);
    const std::string log = read_text(directory.child("status2.log"));
    QOS_CHECK(contains(log, "EPOCH current=2"));
    QOS_CHECK(contains(log, "clean=1"));
  }
}

QOS_TEST(generation_continues_across_a_real_restart) {
  qostest::TempDir directory("multiprocess-restart");
  const std::string store_path = directory.child("store");
  std::uint16_t first_port = 0;
  {
    const std::string port_file = directory.child("port1.txt");
    Child server = Child::spawn(QOSFABRIC_CLI_PATH,
                                {"serve", "--store", store_path, "--port-file", port_file},
                                directory.child("server1.log"));
    QOS_REQUIRE(server.valid());
    QOS_REQUIRE(wait_for_file(port_file, std::chrono::milliseconds(30000)));
    first_port = parse_port(read_text(port_file));

    Child publisher = Child::spawn(QOSFABRIC_PUBLISHER_PATH,
                                   publisher_arguments(std::to_string(first_port),
                                                       directory.child("p.boot"), "r9"),
                                   directory.child("p1.log"));
    QOS_CHECK_EQ(publisher.wait(), 0);
    const std::string log = read_text(directory.child("p1.log"));
    QOS_CHECK(contains(log, "PUBLISH r9 generation=1"));
    server.terminate();
  }

  const std::string port_file2 = directory.child("port2.txt");
  Child server = Child::spawn(QOSFABRIC_CLI_PATH,
                              {"serve", "--store", store_path, "--port-file", port_file2},
                              directory.child("server2.log"));
  QOS_REQUIRE(server.valid());
  QOS_REQUIRE(wait_for_file(port_file2, std::chrono::milliseconds(30000)));
  const std::uint16_t second_port = parse_port(read_text(port_file2));
  QOS_CHECK(second_port != 0);

  Child publisher = Child::spawn(QOSFABRIC_PUBLISHER_PATH,
                                 publisher_arguments(std::to_string(second_port),
                                                     directory.child("p.boot"), "r9"),
                                 directory.child("p2.log"));
  QOS_CHECK_EQ(publisher.wait(), 0);
  const std::string log = read_text(directory.child("p2.log"));
  // The durable generation survived the restart, so the next declaration is
  // generation two even though this is a different server process.
  QOS_CHECK(contains(log, "PUBLISH r9 generation=2"));
  server.terminate();
}
