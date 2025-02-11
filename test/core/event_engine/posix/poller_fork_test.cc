// Copyright 2025 gRPC Authors
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

#include <arpa/inet.h>
#include <fcntl.h>
#include <gmock/gmock.h>
#include <grpc/event_engine/event_engine.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <queue>
#include <utility>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "gmock/gmock.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/event_engine/channel_args_endpoint_config.h"
#include "src/core/lib/event_engine/posix_engine/posix_engine.h"
#include "src/core/lib/event_engine/tcp_socket_utils.h"
#include "src/core/util/wait_for_single_owner.h"
#include "test/core/test_util/port.h"

namespace grpc_event_engine::experimental {

namespace {

class StatusListener {
 public:
  explicit StatusListener(absl::Mutex* mu) : mu_(mu) {}

  absl::Status AwaitStatus() {
    absl::MutexLock lock(mu_);
    mu_->Await({&status_, &decltype(status_)::has_value});
    return std::move(status_).value();
  }

  absl::AnyInvocable<void(absl::Status)> Setter() {
    return [&](absl::Status status) {
      absl::MutexLock lock(mu_);
      status_ = std::move(status);
    };
  }

 private:
  absl::Mutex* mu_;
  std::optional<absl::Status> status_ ABSL_GUARDED_BY(mu_);
};

class RawPosixClient {
 public:
  explicit RawPosixClient(const EventEngine::ResolvedAddress& address) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
      status_ = absl::ErrnoToStatus(errno, "socket call");
    } else {
      int result =
          connect(sockfd, address.address(), sizeof(*address.address()));
      if (result < 0) {
        status_ = absl::ErrnoToStatus(errno, "connect call");
      } else {
        socket_ = sockfd;
        status_ = absl::OkStatus();
      }
    }
  }

  RawPosixClient(const RawPosixClient& /* other */) = delete;

  RawPosixClient(RawPosixClient&& other) noexcept
      : socket_(other.socket_), status_(other.status()) {
    other.socket_ = -1;
    other.status_ = absl::OkStatus();
  }

  ~RawPosixClient() {
    if (socket_ > 0) {
      close(socket_);
      socket_ = -1;
    }
  }

  absl::Status status() const& { return status_; }

  absl::Status&& status() && { return std::move(status_); }

  int socket_fd() const { return socket_; }

 private:
  int socket_ = -1;
  absl::Status status_;
};

class PollerForkTest : public ::testing::Test {
 public:
  void SetUp() override {
    ee_ = GetDefaultEventEngine();
    // Setup listener and establish socket connection, confirm they work
    auto listener_and_address = SetupListener(
        [&](auto endpoint, MemoryAllocator /* memory */) {
          absl::MutexLock lock(&mu_);
          endpoints_.emplace(std::move(endpoint));
        },
        listener_done_.Setter());
    ASSERT_THAT(listener_and_address, absl_testing::IsOk());
    address_ = listener_and_address->second;
    listener_ = std::move(listener_and_address->first);
    RawPosixClient client(listener_and_address->second);
    ASSERT_THAT(client.status(), absl_testing::IsOk());
    // Sanity check - confirm a read operation works
    ASSERT_THAT(SendFromRawToEE(client.socket_fd(), *AwaitEndpoint(), "Hello"),
                absl_testing::IsOk());
  }

  void TearDown() override {
    {
      absl::MutexLock lock(&mu_);
      EXPECT_THAT(endpoints_, ::testing::IsEmpty());
      endpoints_ = {};
    }
    listener_.reset();
    EXPECT_THAT(listener_done_.AwaitStatus(), ::absl_testing::IsOk());
    grpc_core::WaitForSingleOwnerWithTimeout(std::move(ee_),
                                             grpc_core::Duration::Seconds(30));
  }

  std::unique_ptr<EventEngine::Endpoint> AwaitEndpoint() {
    mu_.LockWhen(absl::Condition(
        +[](decltype(endpoints_)* endpoints) { return !endpoints->empty(); },
        &endpoints_));
    std::unique_ptr<EventEngine::Endpoint> endpoint =
        std::move(endpoints_.back());
    endpoints_.pop();
    LOG(INFO) << "Endpoint connected: "
              << ResolvedAddressToNormalizedString(endpoint->GetPeerAddress());
    mu_.Unlock();
    return endpoint;
  }

  PosixEventEngine* ee() { return static_cast<PosixEventEngine*>(ee_.get()); }

  absl::StatusOr<std::pair<std::unique_ptr<EventEngine::Listener>,
                           EventEngine::ResolvedAddress>>
  SetupListener(absl::AnyInvocable<void(std::unique_ptr<EventEngine::Endpoint>,
                                        MemoryAllocator)>
                    on_accept,
                absl::AnyInvocable<void(absl::Status)> on_shutdown) {
    int port = grpc_pick_unused_port_or_die();
    grpc_core::ChannelArgs args;
    auto quota = grpc_core::ResourceQuota::Default();
    args = args.Set(GRPC_ARG_RESOURCE_QUOTA, quota);
    ChannelArgsEndpointConfig config(args);
    std::optional<absl::Status> listener_done;
    std::vector<std::unique_ptr<EventEngine::Endpoint>> endpoints;
    auto listener = ee()->CreateListener(
        std::move(on_accept), std::move(on_shutdown), config,
        std::make_unique<grpc_core::MemoryQuota>("foo"));
    if (!listener.ok()) {
      return std::move(listener).status();
    }
    auto address = URIToResolvedAddress(absl::StrCat("ipv4:127.0.0.1:", port));
    if (!address.ok()) {
      return std::move(address).status();
    }
    auto bound_port = (*listener)->Bind(*address);
    if (!bound_port.ok()) {
      return std::move(bound_port).status();
    }
    auto status = (*listener)->Start();
    if (!status.ok()) {
      return status;
    }
    return std::make_pair(std::move(listener).value(),
                          std::move(address).value());
  }

  absl::Status SendFromRawToEE(int socket_fd, EventEngine::Endpoint& endpoint,
                               absl::string_view data) {
    absl::Mutex mu;
    SliceBuffer buffer;
    StatusListener read_status(&mu);
    EventEngine::Endpoint::ReadArgs read_args = {
        static_cast<int64_t>(data.length())};
    if (endpoint.Read(read_status.Setter(), &buffer, &read_args)) {
      return absl::FailedPreconditionError("Endpoint has pending data");
    }
    ssize_t wrote = write(socket_fd, data.data(), data.size());
    if (wrote < 0) {
      return absl::ErrnoToStatus(errno, "Write to socket");
    }
    if (wrote < data.size()) {
      return absl::DataLossError("Did not write all the data");
    }
    auto status = read_status.AwaitStatus();
    if (!status.ok()) {
      return status;
    }
    if (buffer.Length() != data.size()) {
      return absl::InternalError(absl::StrFormat("Read %ld instead of %ld",
                                                 buffer.Length(), data.size()));
    }
    Slice slice = buffer.TakeFirst();
    if (slice.as_string_view() != data) {
      return absl::InternalError(absl::StrFormat("Read %v, expected %v",
                                                 slice.as_string_view(), data));
    }
    LOG(INFO) << "Read " << slice.as_string_view();
    return absl::OkStatus();
  }

 protected:
  std::shared_ptr<EventEngine> ee_;
  absl::Mutex mu_;
  StatusListener listener_done_{&mu_};
  std::unique_ptr<EventEngine::Listener> listener_;
  std::queue<std::unique_ptr<EventEngine::Endpoint>> endpoints_
      ABSL_GUARDED_BY(&mu_);
  EventEngine::ResolvedAddress address_;
};

}  // namespace

TEST_F(PollerForkTest, ListenerInParent) {
  // Connect before "fork"
  RawPosixClient client(address_);
  ASSERT_THAT(client.status(), absl_testing::IsOk());
  auto endpoint = AwaitEndpoint();
  // Start read and write, cause the fork. Both operations should succeed
  // post-fork.
  FAIL() << "Implement this!";
  ee()->BeforeFork();
  ee()->AfterForkInParent();
  // Read and Write operations work.
  ASSERT_THAT(SendFromRawToEE(client.socket_fd(), *endpoint, "Hello again"),
              absl_testing::IsOk());
}

TEST_F(PollerForkTest, ListenerInChild) {
  // Connect before "fork"
  RawPosixClient client(address_);
  ASSERT_THAT(client.status(), absl_testing::IsOk());
  auto endpoint = AwaitEndpoint();
  ee()->BeforeFork();
  ee()->AfterForkInChild();
  auto failure = SendFromRawToEE(client.socket_fd(), *endpoint, "Hello again");
  ASSERT_THAT(failure,
              absl_testing::StatusIs(absl::StatusCode::kResourceExhausted));
  ASSERT_THAT(failure.message(), ::testing::StartsWith("Handle was shut down"));
}

}  // namespace grpc_event_engine::experimental

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc_init();
  int r = RUN_ALL_TESTS();
  grpc_shutdown();
  return r;
}
