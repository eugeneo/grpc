// Copyright 2024 The gRPC Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef GRPC_SRC_CORE_LIB_EVENT_ENGINE_EXTENSIONS_SYSTEM_API_H
#define GRPC_SRC_CORE_LIB_EVENT_ENGINE_EXTENSIONS_SYSTEM_API_H

#include <grpc/event_engine/event_engine.h>
#include <grpc/support/port_platform.h>

#include "absl/status/status.h"

namespace grpc_event_engine {
namespace experimental {

class FileDescriptor {
 public:
  FileDescriptor() : fd_(-1) {}
  explicit FileDescriptor(int fd) : fd_(fd) {}

  bool ready() const { return fd_ > 0; }
  void invalidate() { fd_ = -1; }
  int fd() const { return fd_; }

 private:
  int fd_;
};

class SystemApi {
 public:
  virtual ~SystemApi() = default;

  // Factories
  virtual FileDescriptor AdoptExternalFd(int fd) const = 0;
  virtual FileDescriptor Socket(int domain, int type, int protocol) const = 0;

  // Functions operating on file descriptors
  virtual int Bind(FileDescriptor fd, const struct sockaddr* addr,
                   socklen_t addrlen) const = 0;
  virtual void Close(FileDescriptor fd) const = 0;
  virtual int Fcntl(FileDescriptor fd, int op, int args) const = 0;
  virtual int GetSockOpt(FileDescriptor fd, int level, int optname,
                         void* optval, socklen_t* optlen) const = 0;
  virtual int GetSockName(FileDescriptor fd, struct sockaddr* addr,
                          socklen_t* addrlen) const = 0;
  virtual int GetPeerName(FileDescriptor fd, struct sockaddr* addr,
                          socklen_t* addrlen) const = 0;
  virtual int Listen(FileDescriptor fd, int backlog) const = 0;
  virtual long RecvMsg(FileDescriptor fd, struct msghdr* msg,
                       int flags) const = 0;
  virtual long SendMsg(FileDescriptor fd, const struct msghdr* message,
                       int flags) const = 0;
  virtual int SetSockOpt(FileDescriptor fd, int level, int optname,
                         const void* optval, socklen_t optlen) const = 0;

  // Return true if SO_REUSEPORT is supported
  virtual bool IsSocketReusePortSupported() const = 0;
  // Tries to set SO_NOSIGPIPE if available on this platform.
  // If SO_NO_SIGPIPE is not available, returns not OK status.
  virtual absl::Status SetSocketNoSigpipeIfPossible(
      FileDescriptor fd) const = 0;
  // Set SO_REUSEPORT
  virtual absl::Status SetSocketReusePort(FileDescriptor fd,
                                          int reuse) const = 0;
  // Set socket to use zerocopy
  virtual absl::Status SetSocketZeroCopy(FileDescriptor fd) const = 0;
  // Set socket to non blocking mode
  virtual absl::Status SetSocketNonBlocking(FileDescriptor fd,
                                            int non_blocking) const = 0;
  // Set socket to close on exec
  virtual absl::Status SetSocketCloexec(FileDescriptor fd,
                                        int close_on_exec) const = 0;
  // Disable nagle algorithm
  virtual absl::Status SetSocketLowLatency(FileDescriptor fd,
                                           int low_latency) const = 0;
  // Set socket to reuse old addresses
  virtual absl::Status SetSocketReuseAddr(FileDescriptor fd,
                                          int reuse) const = 0;
  // Set Differentiated Services Code Point (DSCP)
  virtual absl::Status SetSocketDscp(FileDescriptor fd, int dscp) const = 0;
  // Tries to set IP_PKTINFO if available on this platform. If IP_PKTINFO is not
  // available, returns not OK status.
  virtual absl::Status SetSocketIpPktInfoIfPossible(
      FileDescriptor fd) const = 0;
  // Tries to set IPV6_RECVPKTINFO if available on this platform. If
  // IPV6_RECVPKTINFO is not available, returns not OK status.
  virtual absl::Status SetSocketIpv6RecvPktInfoIfPossible(
      FileDescriptor fd) const = 0;
  // Tries to set the socket's send buffer to given size.
  virtual absl::Status SetSocketSndBuf(FileDescriptor fd,
                                       int buffer_size_bytes) const = 0;
  // Tries to set the socket's receive buffer to given size.
  virtual absl::Status SetSocketRcvBuf(FileDescriptor fd,
                                       int buffer_size_bytes) const = 0;
  // Override default Tcp user timeout values if necessary.
  virtual void TrySetSocketTcpUserTimeout(FileDescriptor fd,
                                          int keep_alive_time_ms,
                                          int keep_alive_timeout_ms,
                                          bool is_client) const = 0;

  // Configure default values for tcp user timeout to be used by client
  // and server side sockets.
  virtual void ConfigureDefaultTcpUserTimeout(bool enable, int timeout,
                                              bool is_client) = 0;
};

}  // namespace experimental
}  // namespace grpc_event_engine

#endif  // GRPC_SRC_CORE_LIB_EVENT_ENGINE_EXTENSIONS_SYSTEM_API_H