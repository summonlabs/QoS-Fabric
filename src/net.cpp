// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "qosfabric/net.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "qosfabric/checked.hpp"
#include "qosfabric/crypto.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace qosfabric {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

constexpr std::size_t kFrameHeaderBytes = 8;

int last_error_code() noexcept {
#ifdef _WIN32
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

void close_native(NativeSocket socket) noexcept {
  if (socket == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

}  // namespace

std::string last_socket_error() {
  const int code = last_error_code();
#ifdef _WIN32
  char* buffer = nullptr;
  const DWORD length = ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string text = "socket error " + std::to_string(code);
  if (length > 0 && buffer != nullptr) {
    text += ": ";
    text.append(buffer, length);
    ::LocalFree(buffer);
  }
  if (text.size() > limits::kMaxErrorDetailBytes) {
    text.resize(limits::kMaxErrorDetailBytes);
  }
  return text;
#else
  return std::string("socket error ") + std::to_string(code) + ": " + std::strerror(code);
#endif
}

Result<void> net_init() {
#ifdef _WIN32
  static const bool initialized = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  if (!initialized) {
    return make_error(ErrorCode::IoError, "socket layer initialization failed");
  }
#endif
  return {};
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) { other.handle_ = -1; }

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = -1;
  }
  return *this;
}

TcpSocket::~TcpSocket() { close(); }

void TcpSocket::close() noexcept {
  if (handle_ >= 0) {
    close_native(static_cast<NativeSocket>(handle_));
    handle_ = -1;
  }
}

Result<void> TcpSocket::set_receive_timeout_ms(std::uint32_t timeout_ms) const {
  if (!valid()) {
    return make_error(ErrorCode::InvalidArgument, "socket is not open");
  }
#ifdef _WIN32
  const DWORD value = static_cast<DWORD>(timeout_ms);
  if (::setsockopt(static_cast<NativeSocket>(handle_), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
    return make_error(ErrorCode::IoError, last_socket_error());
  }
#else
  timeval value{};
  value.tv_sec = static_cast<long>(timeout_ms / 1000u);
  value.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
  if (::setsockopt(static_cast<NativeSocket>(handle_), SOL_SOCKET, SO_RCVTIMEO, &value,
                   sizeof(value)) != 0) {
    return make_error(ErrorCode::IoError, last_socket_error());
  }
#endif
  return {};
}

Result<void> TcpSocket::send_all(const std::vector<std::uint8_t>& bytes) const {
  if (!valid()) {
    return make_error(ErrorCode::InvalidArgument, "socket is not open");
  }
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const std::size_t remaining = bytes.size() - sent;
    const int chunk = static_cast<int>(remaining > 1u << 20 ? 1u << 20 : remaining);
#ifdef _WIN32
    const int written = ::send(static_cast<NativeSocket>(handle_),
                               reinterpret_cast<const char*>(bytes.data() + sent), chunk, 0);
#else
    const ssize_t written = ::send(static_cast<NativeSocket>(handle_),
                                   static_cast<const void*>(bytes.data() + sent), chunk, 0);
#endif
    if (written <= 0) {
      return make_error(ErrorCode::IoError, last_socket_error());
    }
    sent += static_cast<std::size_t>(written);
  }
  return {};
}

Result<std::vector<std::uint8_t>> TcpSocket::recv_exact(std::size_t count) const {
  if (!valid()) {
    return make_error(ErrorCode::InvalidArgument, "socket is not open");
  }
  std::vector<std::uint8_t> out(count);
  std::size_t received = 0;
  while (received < count) {
    const std::size_t remaining = count - received;
    const int chunk = static_cast<int>(remaining > 1u << 20 ? 1u << 20 : remaining);
#ifdef _WIN32
    const int got = ::recv(static_cast<NativeSocket>(handle_),
                           reinterpret_cast<char*>(out.data() + received), chunk, 0);
#else
    const ssize_t got = ::recv(static_cast<NativeSocket>(handle_),
                               static_cast<void*>(out.data() + received), chunk, 0);
#endif
    if (got <= 0) {
      return make_error(ErrorCode::IoError, last_socket_error());
    }
    received += static_cast<std::size_t>(got);
  }
  return out;
}

Result<std::vector<std::uint8_t>> TcpSocket::recv_frame() const {
  const auto header = recv_exact(kFrameHeaderBytes);
  if (!header) {
    return header.error();
  }
  const std::uint32_t length = static_cast<std::uint32_t>(header->at(0)) |
                               (static_cast<std::uint32_t>(header->at(1)) << 8) |
                               (static_cast<std::uint32_t>(header->at(2)) << 16) |
                               (static_cast<std::uint32_t>(header->at(3)) << 24);
  if (length > limits::kMaxWireFrameBytes) {
    // The declared length is rejected before a single byte of body is read or
    // allocated, so a hostile peer cannot drive an allocation.
    return make_error(ErrorCode::TooLarge, "peer declared a frame beyond the wire budget");
  }
  std::size_t total = 0;
  if (!checked_add<std::size_t>(kFrameHeaderBytes, length, total)) {
    return make_error(ErrorCode::Overflow, "frame length arithmetic overflowed");
  }
  std::vector<std::uint8_t> frame;
  frame.reserve(total);
  frame.insert(frame.end(), header->begin(), header->end());
  if (length > 0) {
    const auto body = recv_exact(length);
    if (!body) {
      return body.error();
    }
    frame.insert(frame.end(), body->begin(), body->end());
  }
  return frame;
}

Result<TcpSocket> listen_loopback(std::uint16_t port, std::uint32_t backlog) {
  const auto init = net_init();
  if (!init) {
    return init.error();
  }
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    return make_error(ErrorCode::IoError, last_socket_error());
  }
  TcpSocket result(static_cast<std::intptr_t>(socket));
  const int reuse = 1;
  (void)::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                     sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    return make_error(ErrorCode::IoError, std::string("bind failed: ") + last_socket_error());
  }
  if (::listen(socket, static_cast<int>(backlog)) != 0) {
    return make_error(ErrorCode::IoError, std::string("listen failed: ") + last_socket_error());
  }
  return result;
}

Result<std::uint16_t> local_port(const TcpSocket& socket) {
  if (!socket.valid()) {
    return make_error(ErrorCode::InvalidArgument, "socket is not open");
  }
  sockaddr_in address{};
#ifdef _WIN32
  int length = sizeof(address);
#else
  socklen_t length = sizeof(address);
#endif
  if (::getsockname(static_cast<NativeSocket>(socket.native()),
                    reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return make_error(ErrorCode::IoError, last_socket_error());
  }
  return ntohs(address.sin_port);
}

Result<TcpSocket> accept_connection(const TcpSocket& listener) {
  if (!listener.valid()) {
    return make_error(ErrorCode::InvalidArgument, "listener is not open");
  }
  const NativeSocket accepted =
      ::accept(static_cast<NativeSocket>(listener.native()), nullptr, nullptr);
  if (accepted == kInvalidSocket) {
    return make_error(ErrorCode::IoError, last_socket_error());
  }
  const int nodelay = 1;
  (void)::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
  return TcpSocket(static_cast<std::intptr_t>(accepted));
}

Result<TcpSocket> connect_loopback(std::uint16_t port, std::uint32_t timeout_ms) {
  const auto init = net_init();
  if (!init) {
    return init.error();
  }
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    return make_error(ErrorCode::IoError, last_socket_error());
  }
  TcpSocket result(static_cast<std::intptr_t>(socket));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    return make_error(ErrorCode::IoError, std::string("connect failed: ") + last_socket_error());
  }
  const int nodelay = 1;
  (void)::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
                     sizeof(nodelay));
  if (timeout_ms > 0) {
    (void)result.set_receive_timeout_ms(timeout_ms);
  }
  return result;
}

}  // namespace qosfabric
