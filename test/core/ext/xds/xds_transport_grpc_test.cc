// Copyright 2021 gRPC authors.
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

#include "src/core/ext/xds/xds_transport_grpc.h"

#include <utility>
#include <variant>
#include <vector>

#include "absl/strings/string_view.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <grpc/support/log.h>

#include "src/core/ext/xds/xds_bootstrap_grpc.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "test/core/util/test_config.h"

namespace grpc_core {
namespace testing {
namespace {

using EventHandlerEvent = absl::variant<
    bool, absl::Status,
    std::pair<std::string, RefCountedPtr<XdsTransportFactory::XdsTransport::
                                             StreamingCall::ReadDelayHandle>>>;

class TestEventHandler
    : public XdsTransportFactory::XdsTransport::StreamingCall::EventHandler {
 public:
  explicit TestEventHandler(std::vector<EventHandlerEvent>* events)
      : events_(events) {}

  void OnRequestSent(bool ok) override { events_->emplace_back(ok); }

  void OnRecvMessage(
      absl::string_view payload,
      RefCountedPtr<
          XdsTransportFactory::XdsTransport::StreamingCall::ReadDelayHandle>
          read_delay_handle) override {
    events_->emplace_back(
        std::make_pair(std::string(payload), std::move(read_delay_handle)));
  }

  void OnStatusReceived(absl::Status status) override {
    events_->emplace_back(std::move(status));
  }

 private:
  std::vector<EventHandlerEvent>* events_;
};

TEST(GrpcTransportTest, WaitsWithAdsRead) {
  ExecCtx exec_ctx;
  ChannelArgs args;
  auto factory = MakeOrphanable<GrpcXdsTransportFactory>(args);
  GrpcXdsBootstrap::GrpcXdsServer server;

  absl::Status status;

  auto transport = factory->Create(
      server,
      [](auto s) {
        gpr_log(GPR_ERROR, "%s", std::string(s.message()).c_str());
      },
      &status);
  std::vector<EventHandlerEvent> events;
  auto call = transport->CreateStreamingCall(
      "boop", std::make_unique<TestEventHandler>(&events));

  EXPECT_THAT(events, ::testing::IsEmpty());
}

}  // namespace
}  // namespace testing
}  // namespace grpc_core

int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(&argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  grpc_init();
  const auto result = RUN_ALL_TESTS();
  grpc_shutdown();
  return result;
}
