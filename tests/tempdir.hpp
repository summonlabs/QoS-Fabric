// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A unique, self-cleaning scratch directory for suites that exercise durable
// state. Removal is best effort and never fails a test: a leftover directory
// under the system temporary area is not a product defect.

#ifndef QOSFABRIC_TEST_TEMPDIR_HPP
#define QOSFABRIC_TEST_TEMPDIR_HPP

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

namespace qostest {

class TempDir {
 public:
  explicit TempDir(const char* label) {
    static std::atomic<unsigned> counter{0};
    const unsigned ordinal = counter.fetch_add(1, std::memory_order_relaxed);
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    const std::filesystem::path root = ec ? std::filesystem::path(".") : base;
    path_ = (root / (std::string("qosfabric-test-") + label + "-" + std::to_string(ordinal)))
                .string();
    std::filesystem::remove_all(std::filesystem::path(path_), ec);
    // The directory exists from construction: suites use it for scratch files
    // (child process logs, boot counters) before any store is opened.
    std::filesystem::create_directories(std::filesystem::path(path_), ec);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(path_), ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string child(const char* name) const { return path_ + "/" + name; }

 private:
  std::string path_{};
};

}  // namespace qostest

#endif  // QOSFABRIC_TEST_TEMPDIR_HPP
