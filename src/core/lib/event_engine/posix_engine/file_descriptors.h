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

#ifndef GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_FILE_DESCRIPTORS_H
#define GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_FILE_DESCRIPTORS_H

#include <unordered_set>

#include "absl/types/optional.h"
#include "src/core/util/sync.h"

namespace grpc_event_engine {
namespace experimental {

class SystemApi;

// FD that is locked for use in this thread
class LockedFd {
 public:
  explicit LockedFd(int fd, const SystemApi& system_api);
  ~LockedFd();

  LockedFd(const LockedFd& other) = delete;
  LockedFd(LockedFd&& other) = default;

  int fd() const { return fd_; }

 private:
  int fd_;
  const SystemApi* system_api_;
};

class FileDescriptor {
 public:
  FileDescriptor() = default;
  explicit FileDescriptor(int fd) : fd_(fd) {}

  bool ready() const { return fd_ > 0; }
  void invalidate() { fd_ = -1; }

  // Not meant to use to access FD for I/O. Only used for debug logging.
  int debug_fd() const { return fd_; }

  int fd() const { return fd_; }

 private:
  friend class LockedFd;

  int fd_ = -1;
};

class FileDescriptors {
 public:
  FileDescriptor Add(int fd);
  absl::optional<int> Remove(const FileDescriptor& fd);
  absl::StatusOr<LockedFd> Lock(const FileDescriptor& fd) const;
  std::unordered_set<int> Clear();

 private:
  grpc_core::Mutex list_mu_;
  grpc_core::Mutex rw_locked_fds_mu_;
  std::unordered_set<int> fds_ ABSL_GUARDED_BY(&list_mu_);
};

}  // namespace experimental
}  // namespace grpc_event_engine

#endif  // GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_FILE_DESCRIPTORS_H