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

#include <array>
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "gtest/gtest.h"

#include <grpc/grpc.h>
#include <grpc/support/json.h>

#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gprpp/debug_location.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/json/json.h"
#include "src/core/lib/load_balancing/lb_policy.h"
#include "test/core/client_channel/lb_policy/lb_policy_test_lib.h"
#include "test/core/util/scoped_env_var.h"
#include "test/core/util/test_config.h"

namespace grpc_core {
namespace testing {
namespace {

class PickFirstTest : public LoadBalancingPolicyTest {
 protected:
  PickFirstTest() : lb_policy_(MakeLbPolicy("pick_first")) {}

  static RefCountedPtr<LoadBalancingPolicy::Config> MakePickFirstConfig(
      bool shuffle_address_list) {
    return MakeConfig(Json::FromArray({Json::FromObject({{
        "pick_first",
        Json::FromObject(
            {{"shuffleAddressList", Json::FromBool(shuffle_address_list)}}),
    }})}));
  }

  absl::string_view GetConnectionRequestedAddress(
      absl::Span<const absl::string_view> addresses, bool shuffle,
      SourceLocation location = SourceLocation()) {
    auto status = ApplyUpdate(
        BuildUpdate(addresses, MakePickFirstConfig(shuffle)), lb_policy_.get());
    EXPECT_TRUE(status.ok()) << status;
    absl::string_view result;
    for (absl::string_view address : addresses) {
      auto* subchannel = FindSubchannel(
          address, ChannelArgs().Set(GRPC_ARG_INHIBIT_HEALTH_CHECKING, true));
      EXPECT_NE(subchannel, nullptr)
          << location.file() << ":" << location.line();
      if (subchannel != nullptr && subchannel->ConnectionRequested()) {
        EXPECT_EQ(result.length(), 0)
            << "Connecting to " << address << " and " << result << "\n"
            << location.file() << ":" << location.line();
        result = address;
      }
    }
    return result;
  }

  static std::vector<absl::string_view> ExcludeAddress(
      absl::Span<const absl::string_view> addresses,
      absl::string_view address) {
    std::vector<absl::string_view> filtered;
    for (absl::string_view addr : addresses) {
      if (addr != address) filtered.push_back(addr);
    }
    return filtered;
  }

  std::set<absl::string_view> ResetPickedAddress(
      absl::Span<const absl::string_view> addresses, size_t iterations,
      bool shuffle) {
    std::set<absl::string_view> selected_addresses;
    absl::string_view address_to_ignore;
    for (size_t i = 0; i < iterations; ++i) {
      // We will be keeping track of these picks
      address_to_ignore = GetConnectionRequestedAddress(
          ExcludeAddress(addresses, address_to_ignore), shuffle);
      selected_addresses.insert(address_to_ignore);
      // These pick exclude the address used above to make sure the policy reset
      // the selected address
      address_to_ignore = GetConnectionRequestedAddress(
          ExcludeAddress(addresses, address_to_ignore), shuffle);
    }
    return selected_addresses;
  }

  OrphanablePtr<LoadBalancingPolicy> lb_policy_;
};

TEST_F(PickFirstTest, FirstAddressWorks) {
  // Send an update containing two addresses.
  constexpr std::array<absl::string_view, 2> kAddresses = {
      "ipv4:127.0.0.1:443", "ipv4:127.0.0.1:444"};
  absl::Status status = ApplyUpdate(BuildUpdate(kAddresses), lb_policy_.get());
  EXPECT_TRUE(status.ok()) << status;
  // LB policy should have created a subchannel for both addresses with
  // the GRPC_ARG_INHIBIT_HEALTH_CHECKING channel arg.
  auto* subchannel = FindSubchannel(
      kAddresses[0], ChannelArgs().Set(GRPC_ARG_INHIBIT_HEALTH_CHECKING, true));
  ASSERT_NE(subchannel, nullptr);
  auto* subchannel2 = FindSubchannel(
      kAddresses[1], ChannelArgs().Set(GRPC_ARG_INHIBIT_HEALTH_CHECKING, true));
  ASSERT_NE(subchannel2, nullptr);
  // When the LB policy receives the first subchannel's initial connectivity
  // state notification (IDLE), it will request a connection.
  EXPECT_TRUE(subchannel->ConnectionRequested());
  // This causes the subchannel to start to connect, so it reports CONNECTING.
  subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  // LB policy should have reported CONNECTING state.
  ExpectConnectingUpdate();
  // The second subchannel should not be connecting.
  EXPECT_FALSE(subchannel2->ConnectionRequested());
  // When the first subchannel becomes connected, it reports READY.
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
  // The LB policy will report CONNECTING some number of times (doesn't
  // matter how many) and then report READY.
  auto picker = WaitForConnected();
  ASSERT_NE(picker, nullptr);
  // Picker should return the same subchannel repeatedly.
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(ExpectPickComplete(picker.get()), kAddresses[0]);
  }
}

TEST_F(PickFirstTest, FirstAddressFails) {
  // Send an update containing two addresses.
  constexpr std::array<absl::string_view, 2> kAddresses = {
      "ipv4:127.0.0.1:443", "ipv4:127.0.0.1:444"};
  absl::Status status = ApplyUpdate(BuildUpdate(kAddresses), lb_policy_.get());
  EXPECT_TRUE(status.ok()) << status;
  // LB policy should have created a subchannel for both addresses with
  // the GRPC_ARG_INHIBIT_HEALTH_CHECKING channel arg.
  auto* subchannel = FindSubchannel(
      kAddresses[0], ChannelArgs().Set(GRPC_ARG_INHIBIT_HEALTH_CHECKING, true));
  ASSERT_NE(subchannel, nullptr);
  auto* subchannel2 = FindSubchannel(
      kAddresses[1], ChannelArgs().Set(GRPC_ARG_INHIBIT_HEALTH_CHECKING, true));
  ASSERT_NE(subchannel2, nullptr);
  // When the LB policy receives the first subchannel's initial connectivity
  // state notification (IDLE), it will request a connection.
  EXPECT_TRUE(subchannel->ConnectionRequested());
  // This causes the subchannel to start to connect, so it reports CONNECTING.
  subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  // LB policy should have reported CONNECTING state.
  ExpectConnectingUpdate();
  // The second subchannel should not be connecting.
  EXPECT_FALSE(subchannel2->ConnectionRequested());
  // The first subchannel's connection attempt fails.
  subchannel->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                   absl::UnavailableError("failed to connect"));
  // The LB policy will start a connection attempt on the second subchannel.
  EXPECT_TRUE(subchannel2->ConnectionRequested());
  // This causes the subchannel to start to connect, so it reports CONNECTING.
  subchannel2->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  // The connection attempt succeeds.
  subchannel2->SetConnectivityState(GRPC_CHANNEL_READY);
  // The LB policy will report CONNECTING some number of times (doesn't
  // matter how many) and then report READY.
  auto picker = WaitForConnected();
  ASSERT_NE(picker, nullptr);
  // Picker should return the same subchannel repeatedly.
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(ExpectPickComplete(picker.get()), kAddresses[1]);
  }
}

TEST_F(PickFirstTest, GoesIdleWhenConnectionFailsThenCanReconnect) {
  // Send an update containing two addresses.
  constexpr std::array<absl::string_view, 2> kAddresses = {
      "ipv4:127.0.0.1:443", "ipv4:127.0.0.1:444"};
  absl::Status status = ApplyUpdate(BuildUpdate(kAddresses), lb_policy_.get());
  EXPECT_TRUE(status.ok()) << status;
  // LB policy should have created a subchannel for both addresses with
  // the GRPC_ARG_INHIBIT_HEALTH_CHECKING channel arg.
  auto* subchannel = FindSubchannel(
      kAddresses[0], ChannelArgs().Set(GRPC_ARG_INHIBIT_HEALTH_CHECKING, true));
  ASSERT_NE(subchannel, nullptr);
  auto* subchannel2 = FindSubchannel(
      kAddresses[1], ChannelArgs().Set(GRPC_ARG_INHIBIT_HEALTH_CHECKING, true));
  ASSERT_NE(subchannel2, nullptr);
  // When the LB policy receives the first subchannel's initial connectivity
  // state notification (IDLE), it will request a connection.
  EXPECT_TRUE(subchannel->ConnectionRequested());
  // This causes the subchannel to start to connect, so it reports CONNECTING.
  subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  // LB policy should have reported CONNECTING state.
  ExpectConnectingUpdate();
  // The second subchannel should not be connecting.
  EXPECT_FALSE(subchannel2->ConnectionRequested());
  // When the first subchannel becomes connected, it reports READY.
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
  // The LB policy will report CONNECTING some number of times (doesn't
  // matter how many) and then report READY.
  auto picker = WaitForConnected();
  ASSERT_NE(picker, nullptr);
  // Picker should return the same subchannel repeatedly.
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(ExpectPickComplete(picker.get()), kAddresses[0]);
  }
  // Connection fails.
  subchannel->SetConnectivityState(GRPC_CHANNEL_IDLE);
  // We should see a re-resolution request.
  ExpectReresolutionRequest();
  // LB policy reports IDLE with a queueing picker.
  ExpectStateAndQueuingPicker(GRPC_CHANNEL_IDLE);
  // By checking the picker, we told the LB policy to trigger a new
  // connection attempt, so it should start over with the first
  // subchannel.
  EXPECT_TRUE(subchannel->ConnectionRequested());
  // The subchannel starts connecting.
  subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  // LB policy should have reported CONNECTING state.
  ExpectConnectingUpdate();
  // Subchannel succeeds in connecting.
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
  // LB policy reports READY.
  picker = WaitForConnected();
  ASSERT_NE(picker, nullptr);
  // Picker should return the same subchannel repeatedly.
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(ExpectPickComplete(picker.get()), kAddresses[0]);
  }
}

TEST_F(PickFirstTest, WithShuffle) {
  grpc_core::testing::ScopedExperimentalEnvVar env_var(
      "GRPC_EXPERIMENTAL_PICKFIRST_LB_CONFIG");
  constexpr size_t kPolicyUpdates = 20;
  constexpr std::array<absl::string_view, 6> kAddressUris = {
      "ipv4:127.0.0.1:443", "ipv4:127.0.0.1:444", "ipv4:127.0.0.1:445",
      "ipv4:127.0.0.1:446", "ipv4:127.0.0.1:447", "ipv4:127.0.0.1:448"};
  // With shuffling, different addresses should be returned.
  EXPECT_GT(ResetPickedAddress(kAddressUris, kPolicyUpdates, true).size(), 1);
  // Without shuffling, the same address will always be returned
  EXPECT_EQ(ResetPickedAddress(kAddressUris, kPolicyUpdates, false).size(), 1);
}

TEST_F(PickFirstTest, DisabledExperiment) {
  constexpr size_t kPolicyUpdates = 20;
  constexpr std::array<absl::string_view, 6> kAddressUris = {
      "ipv4:127.0.0.1:443", "ipv4:127.0.0.1:444", "ipv4:127.0.0.1:445",
      "ipv4:127.0.0.1:446", "ipv4:127.0.0.1:447", "ipv4:127.0.0.1:448"};
  // With shuffling, different addresses should be returned.
  EXPECT_EQ(ResetPickedAddress(kAddressUris, kPolicyUpdates, true).size(), 1);
  // Without shuffling, the same address will always be returned
  EXPECT_EQ(ResetPickedAddress(kAddressUris, kPolicyUpdates, false).size(), 1);
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
