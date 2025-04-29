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

#include "test/core/event_engine/posix/dns_server.h"

#include <queue>
#include <thread>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"
#include "absl/synchronization/mutex.h"
#include "src/core/util/notification.h"

namespace grpc_event_engine::experimental {

namespace {

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

class BytePacker {
 public:
  BytePacker& pack8(uint8_t v) {
    data_.emplace_back(v);
    return *this;
  }

  BytePacker& pack16(uint16_t value) { return packMultiByte(htons(value)); }

  BytePacker& pack32(uint32_t value) { return packMultiByte(htonl(value)); }

  std::vector<uint8_t> data() const { return data_; }

  BytePacker& packBytes(absl::Span<const uint8_t> data) {
    pack16(data.size());
    std::copy(data.begin(), data.end(), std::back_inserter(data_));
    return *this;
  }

  BytePacker& packQName(absl::string_view qname) {
    for (absl::string_view segment : absl::StrSplit(qname, ".")) {
      pack8(segment.size());
      std::copy(segment.begin(), segment.end(), std::back_inserter(data_));
    }
    pack8(0x00);
    return *this;
  }

 private:
  template <typename T>
  BytePacker& packMultiByte(T v) {
    const uint8_t* start = reinterpret_cast<const uint8_t*>(&v);
    std::copy(start, start + sizeof(T), std::back_inserter(data_));
    return *this;
  }

  std::vector<uint8_t> data_;
};

class ByteUnpacker {
 public:
  explicit ByteUnpacker(absl::Span<const uint8_t> data) : data_(data) {}

  ByteUnpacker& expect2(uint16_t expected, absl::string_view name) {
    auto value = read2(name);
    if (value.has_value() && *value != expected) {
      status_ = absl::InvalidArgumentError(absl::Substitute(
          "Filed $0: expected: $1, got: $2", name, expected, *value));
    }
    return *this;
  }

  absl::StatusOr<DnsQuestion> query() const {
    if (!status_.ok()) {
      return status_;
    }
    return query_;
  }

  ByteUnpacker& skip2(absl::string_view name) {
    read2(name);
    return *this;
  }

  ByteUnpacker& unpack(uint16_t DnsQuestion::* field, absl::string_view name) {
    auto value = read2(name);
    if (value.has_value()) {
      query_.*field = *value;
    }
    return *this;
  }

  ByteUnpacker& unpack(std::string DnsQuestion::* field,
                       absl::string_view name) {
    if (!status_.ok()) return *this;
    query_.*field = ParseQName(data_, pos_);
    return *this;
  }

 private:
  std::optional<uint16_t> read2(absl::string_view name) {
    if (!status_.ok()) return std::nullopt;
    if (data_.size() < pos_ + 2) {
      status_ = absl::InvalidArgumentError(
          absl::Substitute("Not enough bytes for $0", name));
      return std::nullopt;
    }
    uint16_t value =
        (static_cast<uint16_t>(data_[pos_]) << 8) + data_[pos_ + 1];
    pos_ += 2;
    // Note that this is network byte order. Not casting pointers to avoid
    // UB with unaligned reads.
    return value;
  }

  absl::Span<const uint8_t> data_;
  DnsQuestion query_;
  size_t pos_ = 0;
  absl::Status status_ = absl::OkStatus();
};

absl::StatusOr<DnsQuestion> ParseQuestion(absl::Span<const uint8_t> buffer) {
  return ByteUnpacker(buffer)
      .unpack(&DnsQuestion::id, "ID")
      // Fields below are ignored for now
      .skip2("FLAGS")
      .expect2(1, "QDCOUNT")
      .expect2(0, "ANCOUNT")
      .expect2(0, "NSCOUNT")
      .expect2(0, "ARCOUNT")
      .unpack(&DnsQuestion::qname, "QNAME")
      .unpack(&DnsQuestion::qtype, "QTYPE")
      .unpack(&DnsQuestion::qclass, "QCLASS")
      .query();
}

std::vector<unsigned char> FormatAnswer(const DnsQuestion& query,
                                        absl::Span<const uint8_t> address) {
  return BytePacker()
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
      .packBytes(address)
      .data();
}

}  // namespace

DnsServer::DnsServer(int port, int sockfd)
    : port_(port),
      sockfd_(sockfd),
      background_thread_(&DnsServer::ServerLoop, this, sockfd) {
  running_.WaitForNotification();
}

DnsServer::~DnsServer() {
  done_.Notify();
  background_thread_.join();
}

std::string DnsServer::address() const {
  return absl::StrCat("127.0.0.1:", port_);
}

DnsQuestion DnsServer::NextQuery() {
  grpc_core::MutexLock lock(&mu_);
  while (questions_.empty()) {
    cond_.WaitWithTimeout(&mu_, absl::Milliseconds(50));
  }
  DnsQuestion q = std::move(questions_.front());
  questions_.pop();
  return q;
}

absl::Status DnsServer::Respond(const DnsQuestion& query,
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

void DnsServer::SetResponder(DnsServer::Autoresponder autoresponder) {
  absl::MutexLock lock(&mu_);
  std::queue<DnsQuestion> filtered;
  while (!questions_.empty()) {
    auto address = autoresponder(questions_.front());
    if (address.empty()) {
      filtered.push(std::move(questions_.front()));
    } else {
      auto status = Respond(questions_.front(), address);
      LOG_IF(FATAL, !status.ok()) << status;
    }
    questions_.pop();
  }
  questions_ = std::move(filtered);
  autoresponder_ = std::move(autoresponder);
}

void DnsServer::ServerLoop(int sockfd) {
  running_.Notify();
  std::array<uint8_t, 2048> buffer;
  sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);

  while (!done_.HasBeenNotified()) {
    ssize_t received_bytes =
        recvfrom(sockfd, buffer.data(), buffer.size(), MSG_DONTWAIT,
                 (struct sockaddr*)&client_addr, &client_len);
    if (received_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Don't wait
      absl::SleepFor(absl::Milliseconds(5));
      continue;
    }
    if (received_bytes < 0) {
      LOG(FATAL) << absl::ErrnoToStatus(errno, "Reading from socket");
      return;
    }
    auto query =
        ParseQuestion(absl::Span<const uint8_t>(buffer).first(received_bytes));
    if (!query.ok()) {
      LOG(FATAL) << query.status();
    }
    query->client_addr = client_addr;
    {
      grpc_core::MutexLock lock(&mu_);
      bool responded = false;
      if (autoresponder_) {
        auto result = autoresponder_(*query);
        if (!result.empty()) {
          auto response = Respond(*query, result);
          LOG_IF(FATAL, !response.ok()) << response;
          responded = true;
          break;
        }
      }
      if (!responded) {
        questions_.push(std::move(query).value());
        cond_.SignalAll();
      }
    }
  }
  close(sockfd);
}

absl::StatusOr<DnsServer> DnsServer::Start(int port) {
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

}  // namespace grpc_event_engine::experimental