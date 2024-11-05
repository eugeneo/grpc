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

#include <sys/epoll.h>
#include <sys/socket.h>

#include <utility>

#include "absl/status/statusor.h"

namespace grpc_event_engine {
namespace experimental {

class FileDescriptor {
 public:
  bool ready() const { return fd_ > 0; }
  void invalidate() { fd_ = -1; }
  int fd() const { return fd_; }

 private:
  int fd_ = -1;
};

class SystemApi {
 public:
  virtual ~SystemApi() = default;

  // Factories
  virtual FileDescriptor AdoptExternalFd(int fd) const = 0;
  virtual FileDescriptor eventfd(unsigned int initval, int flags) const = 0;
  virtual FileDescriptor socket(int domain, int type, int protocol) const = 0;
  virtual absl::StatusOr<std::pair<FileDescriptor, FileDescriptor>> pipe()
      const = 0;

  virtual int bind(FileDescriptor fd, const struct sockaddr* addr,
                   socklen_t addrlen) const = 0;
  virtual void close(FileDescriptor fd) const = 0;
#ifdef GRPC_LINUX_EPOLL_CREATE1
  virtual FileDescriptor epoll_create1(int flags) const = 0;
#endif
#ifdef GRPC_LINUX_EPOLL
  virtual FileDescriptor epoll_create(int size) const = 0;
#endif
  virtual int epoll_wait(FileDescriptor epfd, struct epoll_event* events,
                         int maxevents, int timeout) const = 0;
  virtual int epoll_ctl(FileDescriptor epfd, int op, int fd,
                        struct epoll_event* event) const = 0;
  virtual int eventfd_read(FileDescriptor fd) const = 0;
  virtual int eventfd_write(FileDescriptor fd, uint64_t counter) const = 0;

  virtual int fcntl(FileDescriptor fd, int op, int args) const = 0;
  virtual int getsockopt(FileDescriptor fd, int level, int optname,
                         void* optval, socklen_t* optlen) const = 0;
  virtual int getsockname(FileDescriptor fd, struct sockaddr* addr,
                          socklen_t* addrlen) const = 0;
  virtual int getpeername(FileDescriptor fd, struct sockaddr* addr,
                          socklen_t* addrlen) const = 0;
  virtual int listen(FileDescriptor fd, int backlog) const = 0;
  virtual ssize_t read(FileDescriptor fd, void* buf, size_t nbyte) const = 0;
  virtual ssize_t recvmsg(FileDescriptor fd, struct msghdr* msg,
                          int flags) const = 0;
  virtual int setsockopt(FileDescriptor fd, int level, int optname,
                         const void* optval, socklen_t optlen) const = 0;
  virtual ssize_t sendmsg(FileDescriptor fd, const struct msghdr* message,
                          int flags) const = 0;
  virtual void shutdown(FileDescriptor fd, int how) const = 0;
  virtual ssize_t write(FileDescriptor fd, const void* buf,
                        size_t nbyte) const = 0;
};

}  // namespace experimental
}  // namespace grpc_event_engine

#endif  // GRPC_SRC_CORE_LIB_EVENT_ENGINE_EXTENSIONS_SYSTEM_API_H