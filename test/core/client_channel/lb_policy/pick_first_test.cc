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
#include <array>
#include <vector>

#include "absl/algorithm/container.h"
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

  absl::string_view GetConnectingSubchannel(
      absl::Span<const absl::string_view> addresses,
      SourceLocation location = SourceLocation()) {
    for (auto address : addresses) {
      auto* subchannel = FindSubchannel(
          address, ChannelArgs().Set(GRPC_ARG_INHIBIT_HEALTH_CHECKING, true));
      EXPECT_NE(subchannel, nullptr)
          << location.file() << ":" << location.line();
      if (subchannel == nullptr) {
        return absl::string_view();
      } else if (subchannel->ConnectionRequested()) {
        return address;
      }
    }
    return absl::string_view();
  }

  // Gets order the addresses are being picked. Return type is void so
  // assertions can be used
  void GetOrderAddressesArePicked(
      absl::Span<const absl::string_view> addresses,
      std::vector<absl::string_view>* out_address_order) {
    absl::string_view address;
    SubchannelState* subchannel = nullptr;
    out_address_order->clear();
    while (addresses.size() > out_address_order->size()) {
      // All channels except the last one must fail
      if (subchannel != nullptr) {
        subchannel->SetConnectivityState(
            GRPC_CHANNEL_TRANSIENT_FAILURE,
            absl::UnavailableError("failed to connect"));
        subchannel->SetConnectivityState(GRPC_CHANNEL_IDLE);
      }
      address = GetConnectingSubchannel(addresses);
      ASSERT_NE(address, "");
      subchannel = FindSubchannel(
          address, ChannelArgs().Set(GRPC_ARG_INHIBIT_HEALTH_CHECKING, true));
      ASSERT_NE(subchannel, nullptr);
      subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
      ExpectConnectingUpdate();
      ASSERT_EQ(absl::c_find(*out_address_order, address),
                out_address_order->end());
      out_address_order->emplace_back(address);
    }
    subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
    auto picker = WaitForConnected();
    ASSERT_NE(picker, nullptr);
    EXPECT_EQ(ExpectPickComplete(picker.get()), address);
    subchannel->SetConnectivityState(GRPC_CHANNEL_IDLE);
    ExpectReresolutionRequest();
    ExpectStateAndQueuingPicker(GRPC_CHANNEL_IDLE);
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
  testing::ScopedExperimentalEnvVar env_var(
      "GRPC_EXPERIMENTAL_PICKFIRST_LB_CONFIG");
  // 6 addresses have 6! = 720 permutations or 0.1% chance that the shuffle
  // returns initial sequence
  constexpr std::array<absl::string_view, 6> kAddresses = {
      "ipv4:127.0.0.1:443", "ipv4:127.0.0.1:444", "ipv4:127.0.0.1:445",
      "ipv4:127.0.0.1:446", "ipv4:127.0.0.1:447", "ipv4:127.0.0.1:448"};
  absl::Status status = ApplyUpdate(
      BuildUpdate(kAddresses, MakePickFirstConfig(true)), lb_policy_.get());
  EXPECT_TRUE(status.ok()) << status;
  std::vector<absl::string_view> prev_attempt_connect_order;
  GetOrderAddressesArePicked(kAddresses, &prev_attempt_connect_order);
  size_t first_shuffle = 0;
  for (size_t i = 0; i < kAddresses.size(); ++i) {
    if (kAddresses[i] != prev_attempt_connect_order[i]) {
      first_shuffle += 1;
    }
  }
  // There is 0.1% chance this check fails by design. Not an assert to prevent
  // flake
  EXPECT_GT(first_shuffle, 0) << "Addresses were not shuffled";
  size_t shuffle_count = 0;
  // 3 attempts to reduce flakes. Probability of a flake is 0.001%
  for (size_t attempt = 0; attempt < 3; ++attempt) {
    std::vector<absl::string_view> address_order;
    GetOrderAddressesArePicked(kAddresses, &address_order);
    for (size_t i = 0; i < address_order.size(); i++) {
      if (address_order[i] != prev_attempt_connect_order[i]) {
        shuffle_count += 1;
      }
    }
    std::swap(prev_attempt_connect_order, address_order);
  }
  ASSERT_GT(shuffle_count, 0) << "Addresses are not reshuffled";
}

TEST_F(PickFirstTest, DisabledExperiment) {
  constexpr std::array<absl::string_view, 6> kAddresses = {
      "ipv4:127.0.0.1:443", "ipv4:127.0.0.1:444", "ipv4:127.0.0.1:445",
      "ipv4:127.0.0.1:446", "ipv4:127.0.0.1:447", "ipv4:127.0.0.1:448"};
  absl::Status status = ApplyUpdate(
      BuildUpdate(kAddresses, MakePickFirstConfig(true)), lb_policy_.get());
  EXPECT_TRUE(status.ok()) << status;
  std::vector<absl::string_view> prev_attempt_connect_order;
  GetOrderAddressesArePicked(kAddresses, &prev_attempt_connect_order);
  size_t first_shuffle = 0;
  for (size_t i = 0; i < kAddresses.size(); ++i) {
    if (kAddresses[i] != prev_attempt_connect_order[i]) {
      first_shuffle += 1;
    }
  }
  // There is 0.1% chance this check fails by design. Not an assert to prevent
  // flake
  ASSERT_EQ(first_shuffle, 0) << "Addresses were shuffled";
  size_t shuffle_count = 0;
  // 3 attempts to reduce flakes. Probability of a flake is 0.001%
  for (size_t attempt = 0; attempt < 3; ++attempt) {
    std::vector<absl::string_view> address_order;
    GetOrderAddressesArePicked(kAddresses, &address_order);
    for (size_t i = 0; i < address_order.size(); i++) {
      if (address_order[i] != prev_attempt_connect_order[i]) {
        shuffle_count += 1;
      }
    }
    std::swap(prev_attempt_connect_order, address_order);
  }
  ASSERT_EQ(shuffle_count, 0) << "Addresses were reshuffled";
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
