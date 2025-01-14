// Copyright 2025 The gRPC Authors
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

#include <grpc/event_engine/event_engine.h>

#include "absl/log/check.h"
#include "absl/status/status.h"

namespace grpc_event_engine::experimental {

class FileDescriptor {
 public:
  FileDescriptor() = default;
  explicit FileDescriptor(int fd) : fd_(fd) {};
  int fd() const { return fd_; }
  bool ready() const { return fd_ > 0; }

 private:
  int fd_ = 0;
};

enum class OperationResultKind {
  kSuccess,          // Operation does not return a file descriptor and
                     // return value was >= 0. native_result holds the
                     // original return value.
  kError,            // Check native_result and errno for details
  kWrongGeneration,  // System call was not performed because file
                     // descriptor belongs to the wrong generation.
};

template <typename Sink>
void AbslStringify(Sink& sink, OperationResultKind kind) {
  sink.Append(kind == OperationResultKind::kSuccess ? "(Success)"
              : kind == OperationResultKind::kError ? "(Posix Error)"
                                                    : "(Wrong Generation)");
}

// Result of the factory call. kWrongGeneration may happen in the call to
// Accept*
struct FileDescriptorResult {
  OperationResultKind kind;
  // gRPC wrapped FileDescriptor, as described above
  FileDescriptor fd;
  // errno value on call completion, in order to reduce the race conditions
  // from relying on global variable.
  int errno_value;

  static FileDescriptorResult FD(const FileDescriptor& fd) {
    return {OperationResultKind::kSuccess, fd, 0};
  }

  static FileDescriptorResult Error() {
    return {OperationResultKind::kError, {}, errno};
  }

  int operator*() const {
    CHECK_OK(status());
    return fd.fd();
  }

  bool ok() const {
    return kind == OperationResultKind::kSuccess && fd.fd() > 0;
  }

  absl::Status status() const {
    switch (kind) {
      case OperationResultKind::kSuccess:
        return absl::OkStatus();
      case OperationResultKind::kError:
        return absl::ErrnoToStatus(errno_value, "");
      case OperationResultKind::kWrongGeneration:
        return absl::InternalError(
            "File descriptor is from the wrong generation");
    }
  }

  bool IsPosixError(int err) const {
    return kind == OperationResultKind::kError && errno_value == err;
  }
};

class FileDescriptors {
 public:
  FileDescriptorResult Accept(int sockfd, struct sockaddr* addr,
                              socklen_t* addrlen);
  FileDescriptorResult Accept4(int sockfd, EventEngine::ResolvedAddress& addr,
                               int nonblock, int cloexec);

  FileDescriptor Adopt(int fd);

  void Close(const FileDescriptor& fd);

 private:
  FileDescriptorResult RegisterPosixResult(int result);
};

}  // namespace grpc_event_engine::experimental

#endif  // GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_FILE_DESCRIPTORS_H