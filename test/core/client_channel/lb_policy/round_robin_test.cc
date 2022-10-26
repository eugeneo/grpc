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
      size_t channelsCount = 3) {
    LoadBalancingPolicy::UpdateArgs update_args;
    update_args.addresses.emplace();
    for (size_t i = 0; i < channelsCount; ++i) {
      update_args.addresses->emplace_back(MakeAddress(UriForSubchannel(i)),
                                          ChannelArgs());
    }
    auto policy = MakeLbPolicy("round_robin");
    absl::Status status = ApplyUpdate(std::move(update_args), policy.get());
    EXPECT_TRUE(status.ok()) << status;
    EXPECT_EQ(channelsCount, subchannel_pool_.size());
    for (size_t i = 0; i < subchannel_pool_.size(); ++i) {
      ExpectState(GRPC_CHANNEL_CONNECTING);
    }
    return policy;
  }

  void SetSubchannelConnectivityState(
      size_t subchannel, grpc_connectivity_state new_state,
      absl::Status status = absl::OkStatus(),
      SourceLocation location = SourceLocation()) {
    auto it = subchannel_pool_.find(
        {MakeAddress(UriForSubchannel(subchannel)), ChannelArgs()});
    ASSERT_NE(it, subchannel_pool_.end())
        << location.file() << ":" << location.line();
    it->second.SetConnectivityState(new_state, status);
  }

  bool ConnectionRequested(size_t subchannel,
                           SourceLocation location = SourceLocation()) {
    auto it = subchannel_pool_.find(
        {MakeAddress(UriForSubchannel(subchannel)), ChannelArgs()});
    return it->second.ConnectionRequested();
  }

  std::string UriForSubchannel(size_t index) {
    std::stringstream out;
    out << "ipv4:127.0.0.1:44" << (index + 1);
    return out.str();
  }
};

TEST_F(RoundRobinTest, SingleChannel) {
  auto policy = RoundRobinPolicy(1);
  // LB policy should have reported CONNECTING state.
  auto picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectPickQueued(picker.get());
  // LB policy should have requested a connection on this subchannel.
  EXPECT_TRUE(ConnectionRequested(0));

  SetSubchannelConnectivityState(0, GRPC_CHANNEL_CONNECTING);
  picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectPickQueued(picker.get());

  // Channel is ready
  SetSubchannelConnectivityState(0, GRPC_CHANNEL_READY);
  picker = ExpectState(GRPC_CHANNEL_READY);
  // Picker should return the same subchannel repeatedly.
  for (size_t i = 0; i < 3; ++i) {
    ExpectPickComplete(picker.get(), UriForSubchannel(0));
  }
  ExpectNoStateChange();

  // There's a failure
  SetSubchannelConnectivityState(0, GRPC_CHANNEL_TRANSIENT_FAILURE);
  ExpectReresolutionRequest();
  picker = ExpectState(GRPC_CHANNEL_TRANSIENT_FAILURE);
  ExpectPickFail(picker.get());

  // ... and a recovery!
  SetSubchannelConnectivityState(0, GRPC_CHANNEL_READY);
  picker = ExpectState(GRPC_CHANNEL_READY);
  ExpectPickComplete(picker.get(), UriForSubchannel(0));

  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, ThreeSubchannels) {
  auto policy = RoundRobinPolicy(3);
  // LB policy should have reported CONNECTING state.
  auto picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectPickQueued(picker.get());
  // LB policy should have requested a connection on this subchannel.
  EXPECT_TRUE(ConnectionRequested(0));

  for (size_t i = 0; i < 3; ++i) {
    SetSubchannelConnectivityState(i, GRPC_CHANNEL_CONNECTING);
    picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  }
  ExpectPickQueued(picker.get());

  // Only one channel is ready
  SetSubchannelConnectivityState(0, GRPC_CHANNEL_READY);
  picker = ExpectState(GRPC_CHANNEL_READY);
  // Picker should return the same subchannel repeatedly.
  for (size_t i = 0; i < 3; ++i) {
    ExpectPickComplete(picker.get(), UriForSubchannel(0));
  }
  ExpectNoStateChange();

  // All channels ready
  SetSubchannelConnectivityState(1, GRPC_CHANNEL_READY);
  SetSubchannelConnectivityState(2, GRPC_CHANNEL_READY);
  ExpectState(GRPC_CHANNEL_READY);
  picker = ExpectState(GRPC_CHANNEL_READY);

  std::unordered_set<std::string> reportedUris;
  // Picker should return each address once, we do not care about the order.
  for (size_t i = 0; i < 20; ++i) {
    absl::optional<std::string> address;
    GetPickedAddress(picker.get(), address);
    EXPECT_TRUE(address.has_value());
    reportedUris.insert(*address);
  }
  EXPECT_EQ(reportedUris.size(), 3);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_NE(reportedUris.find(UriForSubchannel(i)), reportedUris.end())
        << "Subchannel" << i;
  }
  ExpectNoStateChange();

  // Shutdown channel is not considered
  SetSubchannelConnectivityState(1, GRPC_CHANNEL_TRANSIENT_FAILURE);
  ExpectReresolutionRequest();
  picker = ExpectState(GRPC_CHANNEL_READY);
  ExpectNoStateChange();

  reportedUris.clear();
  for (size_t i = 0; i < 20; ++i) {
    absl::optional<std::string> address;
    GetPickedAddress(picker.get(), address);
    EXPECT_TRUE(address.has_value());
    reportedUris.insert(*address);
  }
  EXPECT_EQ(reportedUris.size(), 2);
  EXPECT_NE(reportedUris.find(UriForSubchannel(0)), reportedUris.end());
  EXPECT_NE(reportedUris.find(UriForSubchannel(2)), reportedUris.end());
  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, OneChannelReady) {
  auto policy = RoundRobinPolicy();
  EXPECT_EQ(3, subchannel_pool_.size());
  ExpectState(GRPC_CHANNEL_CONNECTING);

  SetSubchannelConnectivityState(0, GRPC_CHANNEL_READY);
  auto picker = ExpectState(GRPC_CHANNEL_READY);
  ExpectPickComplete(picker.get(), UriForSubchannel(0));
  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, OneChannelConnecting) {
  auto policy = RoundRobinPolicy();
  auto picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectPickQueued(picker.get());
  EXPECT_EQ(3, subchannel_pool_.size());
  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, OneChannelReadyToIdle) {
  auto policy = RoundRobinPolicy();
  ExpectState(GRPC_CHANNEL_CONNECTING);
  SetSubchannelConnectivityState(0, GRPC_CHANNEL_READY);
  ExpectState(GRPC_CHANNEL_READY);
  SetSubchannelConnectivityState(0, GRPC_CHANNEL_IDLE);
  ExpectReresolutionRequest();
  ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, AllTransitFailure) {
  auto policy = RoundRobinPolicy();
  ExpectState(GRPC_CHANNEL_CONNECTING);
  for (size_t i = 0; i < subchannel_pool_.size() - i; ++i) {
    SetSubchannelConnectivityState(i, GRPC_CHANNEL_TRANSIENT_FAILURE);
    ExpectReresolutionRequest();
    ExpectState(GRPC_CHANNEL_CONNECTING);
  }
  SetSubchannelConnectivityState(subchannel_pool_.size() - 1,
                                 GRPC_CHANNEL_TRANSIENT_FAILURE);
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
