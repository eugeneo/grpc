//
// Copyright 2022 gRPC authors.
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
//

#include <stddef.h>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "gtest/gtest.h"

#include <grpc/grpc.h>

#include "src/core/ext/filters/client_channel/subchannel_pool_interface.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/iomgr/resolved_address.h"
#include "src/core/lib/load_balancing/lb_policy.h"
#include "src/core/lib/resolver/server_address.h"
#include "test/core/client_channel/lb_policy/lb_policy_test_lib.h"
#include "test/core/util/test_config.h"

namespace grpc_core {
namespace testing {
namespace {

class RoundRobinTest : public LoadBalancingPolicyTest {
 public:
  OrphanablePtr<LoadBalancingPolicy> RoundRobinPolicy(
      const absl::Span<const std::string> subchannel_addresses) {
    LoadBalancingPolicy::UpdateArgs update_args;
    update_args.addresses.emplace();
    for (const auto& addr : subchannel_addresses) {
      update_args.addresses->emplace_back(MakeAddress(addr), ChannelArgs());
    }
    auto policy = MakeLbPolicy("round_robin");
    absl::Status status = ApplyUpdate(std::move(update_args), policy.get());
    EXPECT_TRUE(status.ok()) << status;
    EXPECT_EQ(subchannel_addresses.size(), subchannel_pool_.size());
    for (size_t i = 0; i < subchannel_pool_.size(); ++i) {
      ExpectState(GRPC_CHANNEL_CONNECTING);
    }
    return policy;
  }

  // Picker should return each address in any order.
  void ExpectPickAddresses(LoadBalancingPolicy::SubchannelPicker* picker,
                           absl::Span<const std::string> uris,
                           size_t iterations,
                           SourceLocation location = SourceLocation()) {
    std::unordered_set<std::string> reportedUris;
    for (size_t i = 0; i < iterations; ++i) {
      auto address = ExpectPickAddress(picker);
      EXPECT_TRUE(address.has_value())
          << location.file() << ":" << location.line();
      reportedUris.insert(*address);
    }
    EXPECT_EQ(reportedUris.size(), uris.size())
        << location.file() << ":" << location.line();
    for (const std::string& uri : uris) {
      EXPECT_NE(reportedUris.find(uri), reportedUris.end())
          << "Subchannel " << uri << location.file() << ":" << location.line();
    }
  }
};

TEST_F(RoundRobinTest, SingleChannel) {
  std::string uri = "ipv4:127.0.0.1:441";
  auto policy = RoundRobinPolicy({uri});
  // LB policy should have reported CONNECTING state.
  auto picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectPickQueued(picker.get());
  auto subchannel = FindSubchannel(uri);
  ASSERT_NE(subchannel, nullptr);
  // LB policy should have requested a connection on this subchannel.
  EXPECT_TRUE(subchannel->ConnectionRequested());
  subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING, absl::OkStatus());
  picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectPickQueued(picker.get());

  // Subchannel is ready
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY, absl::OkStatus());
  picker = ExpectState(GRPC_CHANNEL_READY);
  // Picker should return the same subchannel repeatedly.
  for (size_t i = 0; i < 3; ++i) {
    ExpectPickComplete(picker.get(), uri);
  }
  ExpectNoStateChange();

  // There's a failure
  subchannel->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                   absl::OkStatus());
  ExpectReresolutionRequest();
  picker = ExpectState(GRPC_CHANNEL_TRANSIENT_FAILURE);
  ExpectPickFail(picker.get());

  // ... and a recovery!
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY, absl::OkStatus());
  picker = ExpectState(GRPC_CHANNEL_READY);
  ExpectPickComplete(picker.get(), uri);

  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, ThreeSubchannels) {
  std::array<std::string, 3> uris = {
      "ipv4:127.0.0.1:441",
      "ipv4:127.0.0.1:442",
      "ipv4:127.0.0.1:443",
  };
  auto policy = RoundRobinPolicy(uris);
  // LB policy should have reported CONNECTING state.
  auto picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectPickQueued(picker.get());
  // LB policy should have requested a connection on this subchannel.
  EXPECT_TRUE(FindSubchannel(uris[0])->ConnectionRequested());

  for (const std::string& uri : uris) {
    auto subchannel = FindSubchannel(uri);
    ASSERT_NE(subchannel, nullptr);
    subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING, absl::OkStatus());
    picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  }
  ExpectPickQueued(picker.get());

  // Only one subchannel is ready
  FindSubchannel(uris[0])->SetConnectivityState(GRPC_CHANNEL_READY,
                                                absl::OkStatus());
  picker = ExpectState(GRPC_CHANNEL_READY);
  // Picker should return the same subchannel repeatedly.
  for (size_t i = 0; i < 3; ++i) {
    ExpectPickComplete(picker.get(), uris[0]);
  }
  ExpectNoStateChange();

  auto second_subchannel = FindSubchannel(uris[1]);
  // All subchannels ready
  second_subchannel->SetConnectivityState(GRPC_CHANNEL_READY, absl::OkStatus());
  FindSubchannel(uris[2])->SetConnectivityState(GRPC_CHANNEL_READY,
                                                absl::OkStatus());
  ExpectState(GRPC_CHANNEL_READY);
  picker = ExpectState(GRPC_CHANNEL_READY);

  ExpectPickAddresses(picker.get(), uris, 20);

  ExpectNoStateChange();

  second_subchannel->SetConnectivityState(GRPC_CHANNEL_IDLE, absl::OkStatus());
  ExpectReresolutionRequest();
  ExpectState(GRPC_CHANNEL_READY);
  ExpectNoStateChange();
  second_subchannel->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                          absl::OkStatus());
  ExpectReresolutionRequest();
  picker = ExpectState(GRPC_CHANNEL_READY);
  ExpectPickAddresses(picker.get(), {uris[0], uris[2]}, 20);

  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, OneChannelReady) {
  auto policy = RoundRobinPolicy({
      "ipv4:127.0.0.1:441",
      "ipv4:127.0.0.1:442",
      "ipv4:127.0.0.1:443",
  });
  EXPECT_EQ(3, subchannel_pool_.size());
  ExpectState(GRPC_CHANNEL_CONNECTING);

  auto subchannel = FindSubchannel("ipv4:127.0.0.1:441");
  ASSERT_NE(subchannel, nullptr);
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY, absl::OkStatus());

  auto picker = ExpectState(GRPC_CHANNEL_READY);
  ExpectPickComplete(picker.get(), "ipv4:127.0.0.1:441");
  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, ConnectingFromStart) {
  auto policy = RoundRobinPolicy({
      "ipv4:127.0.0.1:441",
      "ipv4:127.0.0.1:442",
      "ipv4:127.0.0.1:443",
  });
  auto picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectPickQueued(picker.get());
  EXPECT_EQ(3, subchannel_pool_.size());
  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, OneChannelReadyToIdle) {
  auto policy = RoundRobinPolicy({
      "ipv4:127.0.0.1:441",
      "ipv4:127.0.0.1:442",
      "ipv4:127.0.0.1:443",
  });
  ExpectState(GRPC_CHANNEL_CONNECTING);
  auto subchannel = FindSubchannel("ipv4:127.0.0.1:441"

  );
  ASSERT_NE(subchannel, nullptr);
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY, absl::OkStatus());
  ExpectState(GRPC_CHANNEL_READY);
  subchannel->SetConnectivityState(GRPC_CHANNEL_IDLE, absl::OkStatus());
  ExpectReresolutionRequest();
  ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, AllTransientFailure) {
  std::array<std::string, 3> uris = {
      "ipv4:127.0.0.1:441",
      "ipv4:127.0.0.1:442",
      "ipv4:127.0.0.1:443",
  };
  auto policy = RoundRobinPolicy(uris);
  ExpectState(GRPC_CHANNEL_CONNECTING);
  for (size_t i = 0; i < uris.size() - i; ++i) {
    auto subchannel = FindSubchannel(uris[i]);
    ASSERT_NE(subchannel, nullptr);
    subchannel->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                     absl::OkStatus());
    ExpectReresolutionRequest();
    ExpectState(GRPC_CHANNEL_CONNECTING);
  }
  FindSubchannel(uris[2])->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                                absl::OkStatus());
  ExpectReresolutionRequest();
  ExpectState(GRPC_CHANNEL_TRANSIENT_FAILURE);
  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, NoChannels) {
  auto policy = MakeLbPolicy("round_robin");
  LoadBalancingPolicy::UpdateArgs update_args;
  update_args.addresses.emplace();
  absl::Status status = ApplyUpdate(std::move(update_args), policy.get());
  EXPECT_TRUE(absl::IsUnavailable(status));
}

}  // namespace
}  // namespace testing
}  // namespace grpc_core

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc_init();
  int ret = RUN_ALL_TESTS();
  grpc_shutdown();
  return ret;
}
