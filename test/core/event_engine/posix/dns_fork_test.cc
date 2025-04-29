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

#include <absl/container/inlined_vector.h>
#include <grpc/event_engine/event_engine.h>
#include <grpc/grpc.h>
#include <grpc/support/port_platform.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"
#include "absl/types/span.h"
#include "absl/utility/utility.h"
#include "gmock/gmock.h"
#include "src/core/lib/event_engine/ares_resolver.h"
#include "src/core/lib/event_engine/grpc_polled_fd.h"
#include "src/core/lib/event_engine/posix_engine/posix_engine.h"
#include "src/core/lib/event_engine/tcp_socket_utils.h"
#include "src/core/lib/iomgr/port.h"
#include "src/core/util/notification.h"
#include "test/core/test_util/port.h"
#include "test/core/test_util/test_config.h"

namespace grpc_event_engine::experimental {

struct DnsQuery {
  uint16_t id;
  std::string qname;
  uint16_t qtype;
  uint16_t qclass;
  sockaddr_in client_addr;
};

struct DNSRecord {
  absl::Span<uint8_t> rdata;  // Raw resource data (IPv4 or IPv6 address)
};

class DnsServer {
 public:
  static absl::StatusOr<DnsServer> StartDnsServer();

  DnsServer(int port, int sockfd)
      : port_(port),
        sockfd_(sockfd),
        background_thread_(&DnsServer::ServerLoop, this, sockfd) {
    running_.WaitForNotification();
  }

  ~DnsServer() {
    close(sockfd_);
    done_.Notify();
    background_thread_.join();
  }

  std::string address() const { return absl::StrCat("127.0.0.1:", port_); }

  DnsQuery NextQuery() {
    grpc_core::MutexLock lock(&mu_);
    while (queries_.empty()) {
      cond_.WaitWithTimeout(&mu_, absl::Milliseconds(50));
    }
    DnsQuery q = std::move(queries_.front());
    queries_.pop();
    return q;
  }

  absl::Status RespondIPv4(const DnsQuery& query,
                           absl::Span<const uint8_t> answer) {
    auto packet = FormatAnswer(query, answer);
    ssize_t sent = sendto(sockfd_, packet.data(), packet.size(), 0,
                          reinterpret_cast<const sockaddr*>(&query.client_addr),
                          sizeof(query.client_addr));
    if (sent < 0) {
      return absl::ErrnoToStatus(errno, "Sending response");
    }
    return absl::OkStatus();
  }

 private:
  void ServerLoop(int sockfd) {
    running_.Notify();
    std::array<uint8_t, 2048> buffer;
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (!done_.HasBeenNotified()) {
      ssize_t received_bytes =
          recvfrom(sockfd, buffer.data(), buffer.size(), 0,
                   (struct sockaddr*)&client_addr, &client_len);
      if (received_bytes < 0) {
        LOG(FATAL) << absl::ErrnoToStatus(errno, "Reading from socket");
        return;
      }
      auto query = ParseDnsQuery(
          absl::Span<const uint8_t>(buffer).first(received_bytes));
      if (!query.ok()) {
        LOG(FATAL) << query.status();
      }
      query->client_addr = client_addr;
      {
        grpc_core::MutexLock lock(&mu_);
        queries_.push(std::move(query).value());
        cond_.SignalAll();
      }
    }
    close(sockfd);
  }

  std::string ParseQName(absl::Span<const uint8_t> buffer, size_t& pos) {
    absl::InlinedVector<std::string, 10> qname;
    size_t label_length = static_cast<size_t>(buffer[pos++]);
    while (label_length != 0) {
      auto range = buffer.subspan(pos, label_length);
      std::string s(range.begin(), range.end());
      qname.emplace_back(std::move(s));
      pos += label_length;
      label_length = static_cast<size_t>(buffer[pos++]);
    }
    return absl::StrJoin(qname, ".");
  }

  class Unpacker {
   public:
    explicit Unpacker(absl::Span<const uint8_t> data) : data_(data) {}

    Unpacker& unpack(uint16_t DnsQuery::* field, absl::string_view name) {
      if (!status_.ok()) return *this;
      if (data_.size() < pos_ + 2) {
        status_ = absl::InvalidArgumentError(
            absl::Substitute("Not enough bytes for $0", name));
      } else {
        // Note that this is network byte order. Not casting pointers to avoid
        // UB with unaligned reads.
        query_.*field =
            (static_cast<uint16_t>(data_[pos_]) << 8) + data_[pos_ + 1];
      }
      pos_ += 2;
      return *this;
    }

    Unpacker& skip2(absl::string_view name) {
      if (!status_.ok()) return *this;
      if (data_.size() < pos_ + 2) {
        status_ = absl::InvalidArgumentError(
            absl::Substitute("Not enough bytes for $0", name));
      }
      pos_ += 2;
      return *this;
    }

    absl::StatusOr<DnsQuery> query() const {
      if (status_.ok()) {
        return query_;
      } else {
        return status_;
      }
    }

   private:
    absl::Span<const uint8_t> data_;
    DnsQuery query_;
    size_t pos_ = 0;
    absl::Status status_ = absl::OkStatus();
  };

  // Generated by Gemini, good enough for a test!
  absl::StatusOr<DnsQuery> ParseDnsQuery(absl::Span<const uint8_t> buffer) {
    DnsQuery query;
    size_t offset = 0;
    const uint8_t* data = buffer.data();

    // 1. Parse the ID (2 bytes)
    if (buffer.size() < 2) {
      return absl::InvalidArgumentError("Error: Query too short for id");
    }
    query.id = ntohs(*(reinterpret_cast<const uint16_t*>(data + offset)));
    offset += 2;

    // 2. Parse Flags (2 bytes) - We don't need to interpret them for this
    // simple parser
    offset += 2;

    // 3. Parse Question Count (2 bytes) - Should be 1 for a simple query
    if (buffer.size() < offset + 2) {
      return absl::InvalidArgumentError(
          "Error: Query too short for question count");
    }
    uint16_t qdcount =
        ntohs(*(reinterpret_cast<const uint16_t*>(data + offset)));
    offset += 2;
    if (qdcount != 1) {
      std::cerr << "Warning: Expected 1 question, got " << qdcount << std::endl;
      // We'll still try to parse the first question
    }
    auto q = Unpacker(buffer)
                 .unpack(&DnsQuery::id, "id")
                 .skip2("flags")
                 .unpack(&DnsQuery::)
                 .query();
    LOG(INFO) << q->id << " " << query.id;

    // Skip Answer, Authority, and Additional Record Counts (each 2 bytes)
    offset += 2 + 2 + 2;

    // 4. Parse QNAME (variable length)
    query.qname = ParseQName(buffer, offset);
    if (query.qname.empty()) {
      return absl::InvalidArgumentError("Error: Query too short for QNAME");
    }

    // 5. Parse QTYPE (2 bytes) - Should be 1 for A record
    if (buffer.size() < offset + 2) {
      return absl::InvalidArgumentError("Error: Query too short for QTYPE");
    }
    query.qtype = ntohs(*(reinterpret_cast<const uint16_t*>(data + offset)));
    offset += 2;

    // 6. Parse QCLASS (2 bytes) - Should be 1 for IN (Internet) class
    if (buffer.size() < offset + 2) {
      return absl::InvalidArgumentError("Error: Query too short for QCLASS");
    }
    query.qclass = ntohs(*(reinterpret_cast<const uint16_t*>(data + offset)));
    return query;
  }

  class Packer {
   public:
    Packer& pack8(uint8_t v) {
      data_.emplace_back(v);
      return *this;
    }

    Packer& pack16(uint16_t value) { return packMultiByte(htons(value)); }

    Packer& pack32(uint32_t value) { return packMultiByte(htonl(value)); }

    std::vector<uint8_t> data() const { return data_; }

    Packer& packBytes(absl::Span<const uint8_t> data) {
      pack16(data.size());
      std::copy(data.begin(), data.end(), std::back_inserter(data_));
      return *this;
    }

    Packer& packQName(absl::string_view qname) {
      for (absl::string_view segment : absl::StrSplit(qname, ".")) {
        pack8(segment.size());
        std::copy(segment.begin(), segment.end(), std::back_inserter(data_));
      }
      pack8(0x00);
      return *this;
    }

   private:
    template <typename T>
    Packer& packMultiByte(T v) {
      const uint8_t* start = reinterpret_cast<const uint8_t*>(&v);
      std::copy(start, start + sizeof(T), std::back_inserter(data_));
      return *this;
    }

    std::vector<uint8_t> data_;
  };

  std::vector<unsigned char> FormatAnswer(const DnsQuery& query,
                                          absl::Span<const uint8_t> answer) {
    return Packer()
        .pack16(query.id)        // ID
        .pack16(0x8000)          // FLAGS
        .pack16(1)               // QDCOUNT
        .pack16(1)               // ANCOUNT
        .pack16(0)               // NSCOUNT
        .pack16(0)               // ARCOUNT
        .packQName(query.qname)  // Query QNAME
        .pack16(query.qtype)     // QTYPE
        .pack16(query.qclass)    // QCLASS
        .pack16(0xC00C)          // Answer QNAME - pointer
        .pack16(query.qtype)     // QTYPE
        .pack16(query.qclass)    // QCLASS
        .pack32(2000)            // TTL
        .packBytes(answer)
        .data();
  }

  int port_;
  int sockfd_;
  grpc_core::Notification done_;
  grpc_core::Notification running_;
  grpc_core::Mutex mu_;
  grpc_core::CondVar cond_;
  std::thread background_thread_;
  std::queue<DnsQuery> queries_ ABSL_GUARDED_BY(&mu_);
};

absl::StatusOr<DnsServer> StartDnsServer() {
  int port = grpc_pick_unused_port_or_die();
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    return absl::ErrnoToStatus(errno, "Error creating socket");
  }
  sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  server_addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    auto status = absl::ErrnoToStatus(errno, "Error binding socket");
    close(sockfd);
    return status;
  }
  return absl::StatusOr<DnsServer>(absl::in_place, port, sockfd);
}

#ifdef GRPC_ENABLE_FORK_SUPPORT

class DnsForkTest : public testing::Test {
 protected:
  void SetUp() override {
#if GRPC_POSIX_SOCKET_ARES_EV_DRIVER && GRPC_ARES
    ASSERT_TRUE(AresInit().ok());
#endif  // GRPC_POSIX_SOCKET_ARES_EV_DRIVER && GRPC_ARES
    event_engine_ =
        std::static_pointer_cast<PosixEventEngine>(GetDefaultEventEngine());
    ASSERT_NE(event_engine_, nullptr);
  }

  void TearDown() override {
    factory_.reset();
#if GRPC_POSIX_SOCKET_ARES_EV_DRIVER && GRPC_ARES
    AresShutdown();
#endif  // GRPC_POSIX_SOCKET_ARES_EV_DRIVER && GRPC_ARES
  }

  std::shared_ptr<PosixEventEngine> event_engine_;
  std::unique_ptr<GrpcPolledFdFactory> factory_;
};

TEST_F(DnsForkTest, DnsLookupAcrossForkInParent) {
  auto dns_server = StartDnsServer();
  auto resolver =
      event_engine_->GetDNSResolver({.dns_server = dns_server->address()});
  ASSERT_TRUE(resolver.ok()) << resolver.status();
  grpc_core::Notification notification;
  absl::StatusOr<std::vector<std::string>> lookup_result;
  resolver->get()->LookupHostname(
      [&](const auto& addresses) {
        if (addresses.ok()) {
          lookup_result.emplace(addresses->size());
          for (const auto& address : addresses.value()) {
            auto resolved = ResolvedAddressToString(address);
            lookup_result->emplace_back(resolved.ok()
                                            ? resolved.value()
                                            : resolved.status().ToString());
            LOG(INFO) << lookup_result->back();
          }
        } else {
          lookup_result = std::move(addresses).status();
        }
        notification.Notify();
      },
      "fork_test", "443");
  while (!notification.HasBeenNotified()) {
    auto query = dns_server->NextQuery();
    std::pair<std::string_view, std::string_view> name_rest =
        absl::StrSplit(query.qname, ".");
    if (name_rest.first == "fork_test") {
      if (query.qtype == 1) {
        auto status = dns_server->RespondIPv4(query, {1, 1, 1, 1});
        ASSERT_TRUE(status.ok()) << status;
      } else if (query.qtype == 28) {
        auto status = dns_server->RespondIPv4(
            query, {0x20, 0x01, 0x0d, 0xb8, 0x85, 0xa3, 0x00, 0x00, 0x00, 0x00,
                    0x8a, 0x2e, 0x03, 0x70, 0x73, 0x34});
        ASSERT_TRUE(status.ok()) << status;
      } else {
        LOG(FATAL) << query.qtype;
      }
    }
  }
  notification.WaitForNotification();
  ASSERT_TRUE(lookup_result.ok()) << lookup_result.status();
  EXPECT_THAT(lookup_result.value(), ::testing::UnorderedElementsAre("boop"));
  // Send request
  // Fork
  // Send response
  // Verify response was received
}

TEST_F(DnsForkTest, DnsLookupAcrossForkInChild) {
  // Send request1
  // Fork
  // Send response1
  // Verify resolver reported error (unless retry?)
  // Send request2
  // Send response2
  // Response was received
}

#else  // GRPC_ENABLE_FORK_SUPPORT

TEST(AresResolverTest, Skipped) { GTEST_SKIP() << "Fork is disabled"; }

#endif  // GRPC_ENABLE_FORK_SUPPORT

}  // namespace grpc_event_engine::experimental

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc_init();
  auto result = RUN_ALL_TESTS();
  grpc_shutdown();
  return result;
}