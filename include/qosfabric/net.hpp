// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal blocking TCP transport for the publisher protocol.
//
// This is deliberately small: it exists so that capability publication is a
// real framed exchange between real operating-system processes over a real
// socket, not an in-process function call standing in for one. It is loopback
// capable, and it makes no multi-node, multi-switch or fabric-hardware claim.

#ifndef QOSFABRIC_NET_HPP
#define QOSFABRIC_NET_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "qosfabric/error.hpp"
#include "qosfabric/limits.hpp"

namespace qosfabric {

// Initializes the platform socket layer once per process. Idempotent.
Result<void> net_init();

class TcpSocket {
 public:
  TcpSocket() noexcept = default;
  TcpSocket(std::intptr_t handle) noexcept : handle_(handle) {}
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;
  TcpSocket(TcpSocket&& other) noexcept;
  TcpSocket& operator=(TcpSocket&& other) noexcept;
  ~TcpSocket();

  [[nodiscard]] bool valid() const noexcept { return handle_ >= 0; }
  [[nodiscard]] std::intptr_t native() const noexcept { return handle_; }
  void close() noexcept;

  Result<void> send_all(const std::vector<std::uint8_t>& bytes) const;
  Result<std::vector<std::uint8_t>> recv_exact(std::size_t count) const;
  // Reads exactly one framed message straight off the stream, allocating only
  // after the declared length has been checked against the wire budget.
  Result<std::vector<std::uint8_t>> recv_frame() const;
  Result<void> set_receive_timeout_ms(std::uint32_t timeout_ms) const;

 private:
  std::intptr_t handle_{-1};
};

// Binds and listens on loopback. Port zero selects an ephemeral port.
Result<TcpSocket> listen_loopback(std::uint16_t port, std::uint32_t backlog);
[[nodiscard]] Result<std::uint16_t> local_port(const TcpSocket& socket);
Result<TcpSocket> accept_connection(const TcpSocket& listener);
Result<TcpSocket> connect_loopback(std::uint16_t port, std::uint32_t timeout_ms);
// Reports the error text of the most recent socket failure on this thread.
[[nodiscard]] std::string last_socket_error();

}  // namespace qosfabric

#endif  // QOSFABRIC_NET_HPP
