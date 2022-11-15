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
  RoundRobinTest() : policy(MakeLbPolicy("round_robin")) {}

  LoadBalancingPolicy::UpdateArgs BuildUpdateArgs(
      absl::Span<const absl::string_view> subchannel_addresses) {
    LoadBalancingPolicy::UpdateArgs update_args;
    update_args.addresses.emplace();
    for (const auto& addr : subchannel_addresses) {
      update_args.addresses->emplace_back(MakeAddress(addr), ChannelArgs());
    }
    return update_args;
  }

  void ExpectAllChannelsConnecting() {
    for (size_t i = 0; i < subchannel_pool_.size(); ++i) {
      ExpectState(GRPC_CHANNEL_CONNECTING);
    }
  }

  // Picker should return each address in any order.
  void ExpectRoundRobinPicks(LoadBalancingPolicy::SubchannelPicker* picker,
                             absl::Span<const absl::string_view> uris,
                             size_t iterations_per_uri = 3,
                             SourceLocation location = SourceLocation()) {
    int expected = -1;
    for (size_t i = 0; i < iterations_per_uri * uris.size(); ++i) {
      auto address = ExpectPickComplete(picker);
      ASSERT_TRUE(address.has_value())
          << location.file() << ":" << location.line();
      int ind = std::find(uris.begin(), uris.end(), *address) - uris.begin();
      ASSERT_LT(ind, uris.size()) << "Missing " << *address << "\n"
                                  << location.file() << ":" << location.line();
      if (expected >= 0) {
        EXPECT_EQ(ind, expected)
            << "Got " << *address << ", expected " << uris[ind] << "\n"
            << location.file() << ":" << location.line();
      }
      expected = (ind + 1) % uris.size();
    }
  }

  OrphanablePtr<LoadBalancingPolicy> policy;
};

const absl::string_view kFirstAddress = "ipv4:127.0.0.1:441";
const absl::string_view kSecondAddress = "ipv4:127.0.0.1:442";
const absl::string_view kThirdAddress = "ipv4:127.0.0.1:443";

TEST_F(RoundRobinTest, SingleAddress) {
  auto status = ApplyUpdate(BuildUpdateArgs({kFirstAddress}), policy.get());

  ASSERT_TRUE(status.ok()) << status;
  EXPECT_EQ(subchannel_pool_.size(), 1);

  ExpectState(GRPC_CHANNEL_CONNECTING);

  // LB policy should have reported CONNECTING state.
  auto picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectPickQueued(picker.get());
  auto subchannel = FindSubchannel(kFirstAddress);
  ASSERT_NE(subchannel, nullptr);
  // LB policy should have requested a connection on this subchannel.
  EXPECT_TRUE(subchannel->ConnectionRequested());
  subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectPickQueued(picker.get());

  // Subchannel is ready
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
  picker = ExpectState(GRPC_CHANNEL_READY);
  // Picker should return the same subchannel repeatedly.
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(ExpectPickComplete(picker.get()), kFirstAddress);
  }

  subchannel->SetConnectivityState(GRPC_CHANNEL_IDLE);
  ExpectReresolutionRequest();
  ExpectState(GRPC_CHANNEL_CONNECTING);

  subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectNoStateChange();

  // There's a failure
  subchannel->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                   absl::UnavailableError("a test"));
  ExpectReresolutionRequest();
  absl::Status expected_status = absl::UnavailableError(
      "connections to all backends failing; last error: UNAVAILABLE: a test");
  picker = ExpectState(GRPC_CHANNEL_TRANSIENT_FAILURE, expected_status);

  auto pick_result = DoPick(picker.get());
  ASSERT_TRUE(absl::holds_alternative<LoadBalancingPolicy::PickResult::Fail>(
      pick_result.result));

  status = absl::get<LoadBalancingPolicy::PickResult::Fail>(pick_result.result)
               .status;
  EXPECT_EQ(status, expected_status);

  subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  ExpectState(GRPC_CHANNEL_TRANSIENT_FAILURE, expected_status);
  ExpectNoStateChange();

  // ... and a recovery!
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
  picker = ExpectState(GRPC_CHANNEL_READY);
  EXPECT_EQ(ExpectPickComplete(picker.get()), kFirstAddress);

  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, ThreeAddresses) {
  auto status = ApplyUpdate(BuildUpdateArgs({
                                kFirstAddress,
                                kSecondAddress,
                                kThirdAddress,
                            }),
                            policy.get());

  ASSERT_TRUE(status.ok()) << status;
  EXPECT_EQ(subchannel_pool_.size(), 3);
  for (int i = 0; i < 3; i++) {
    ExpectState(GRPC_CHANNEL_CONNECTING);
  }

  // LB policy should have reported CONNECTING state.
  auto picker = ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectPickQueued(picker.get());

  std::array<SubchannelState*, 3> subchannels = {
      FindSubchannel(kFirstAddress),
      FindSubchannel(kSecondAddress),
      FindSubchannel(kThirdAddress),
  };

  for (auto subchannel : subchannels) {
    ASSERT_NE(subchannel, nullptr);
    EXPECT_TRUE(subchannel->ConnectionRequested());
  }

  for (auto subchannel : subchannels) {
    subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  }

  for (int i = 0; i < subchannels.size(); i++) {
    subchannels[i]->SetConnectivityState(GRPC_CHANNEL_READY);
    // For the first subchannel, we use WaitForConnected() to drain any queued
    // CONNECTING updates.  For each successive subchannel, we can read just
    // one READY update at a time.
    auto picker = i == 0 ? WaitForConnected() : ExpectState(GRPC_CHANNEL_READY);
    ASSERT_NE(picker, nullptr);
    ExpectRoundRobinPicks(picker.get(),
                          absl::Span<const absl::string_view>(
                              {kFirstAddress, kSecondAddress, kThirdAddress})
                              .subspan(0, i + 1));
  }

  ExpectNoStateChange();

  subchannels[1]->SetConnectivityState(GRPC_CHANNEL_IDLE);
  ExpectReresolutionRequest();
  picker = ExpectState(GRPC_CHANNEL_READY);
  ExpectRoundRobinPicks(picker.get(), {kFirstAddress, kThirdAddress});
  ExpectNoStateChange();

  subchannels[1]->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  picker = ExpectState(GRPC_CHANNEL_READY);
  ExpectNoStateChange();

  subchannels[1]->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                       absl::UnknownError("This is a test"));
  ExpectReresolutionRequest();
  picker = ExpectState(GRPC_CHANNEL_READY);
  ExpectRoundRobinPicks(picker.get(), {kFirstAddress, kThirdAddress});

  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, OneChannelReady) {
  auto subchannel = CreateSubchannel(kFirstAddress);
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY);

  auto status = ApplyUpdate(BuildUpdateArgs({
                                kFirstAddress,
                                kSecondAddress,
                                kThirdAddress,
                            }),
                            policy.get());
  ASSERT_TRUE(status.ok()) << status;
  ExpectConnectingUpdate();
  ExpectState(GRPC_CHANNEL_READY);
  ExpectState(GRPC_CHANNEL_READY);
  auto picker = ExpectState(GRPC_CHANNEL_READY);
  ExpectRoundRobinPicks(picker.get(), {kFirstAddress});
  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, AllTransientFailure) {
  auto status = ApplyUpdate(BuildUpdateArgs({
                                kFirstAddress,
                                kSecondAddress,
                                kThirdAddress,
                            }),
                            policy.get());
  ASSERT_TRUE(status.ok()) << status;
  for (int i = 0; i < 3; i++) {
    ExpectState(GRPC_CHANNEL_CONNECTING);
  }
  ExpectState(GRPC_CHANNEL_CONNECTING);
  for (auto address : {kFirstAddress, kSecondAddress}) {
    auto subchannel = FindSubchannel(address);
    ASSERT_NE(subchannel, nullptr);
    subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
    ExpectState(GRPC_CHANNEL_READY);
    subchannel->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                     absl::UnknownError("error1"));
    ExpectReresolutionRequest();
    ExpectState(GRPC_CHANNEL_CONNECTING);
  }
  auto third_subchannel = FindSubchannel(kThirdAddress);
  ASSERT_NE(third_subchannel, nullptr);
  third_subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
  ExpectState(GRPC_CHANNEL_READY);

  third_subchannel->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                         absl::UnknownError("error2"));
  ExpectReresolutionRequest();
  ExpectState(
      GRPC_CHANNEL_TRANSIENT_FAILURE,
      absl::UnavailableError(
          "connections to all backends failing; last error: UNKNOWN: error2"));
  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, EmptyAddressList) {
  LoadBalancingPolicy::UpdateArgs update_args;
  update_args.resolution_note = "This is a test";
  update_args.addresses.emplace();
  absl::Status status = ApplyUpdate(std::move(update_args), policy.get());
  EXPECT_TRUE(absl::IsUnavailable(status));
  WaitForConnectionFailed([](const absl::Status& status) {
    EXPECT_EQ(status,
              absl::UnavailableError("empty address list: This is a test"));
  });
  ExpectNoStateChange();
}

TEST_F(RoundRobinTest, AddressListChange) {
  // One channel that is ready. Gets picked all the time.
  auto status = ApplyUpdate(BuildUpdateArgs({kFirstAddress}), policy.get());
  ASSERT_TRUE(status.ok()) << status;
  ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectState(GRPC_CHANNEL_CONNECTING);
  ExpectNoStateChange();
  FindSubchannel(kFirstAddress, {})->SetConnectivityState(GRPC_CHANNEL_READY);
  auto picker = ExpectState(GRPC_CHANNEL_READY);
  auto picked = ExpectPickComplete(picker.get());
  EXPECT_EQ(*picked, kFirstAddress);
  ExpectNoStateChange();

  // Second channel added, connecting. Only the first channel gets picked still
  status = ApplyUpdate(BuildUpdateArgs({kFirstAddress, kSecondAddress}),
                       policy.get());
  ASSERT_TRUE(status.ok()) << status;
  ExpectState(GRPC_CHANNEL_READY);
  picker = ExpectState(GRPC_CHANNEL_READY);
  EXPECT_EQ(*ExpectPickComplete(picker.get()), kFirstAddress);
  EXPECT_EQ(*ExpectPickComplete(picker.get()), kFirstAddress);
  ExpectNoStateChange();

  // Second channel ready. Both channels are now picked.
  FindSubchannel(kSecondAddress, {})->SetConnectivityState(GRPC_CHANNEL_READY);
  picker = ExpectState(GRPC_CHANNEL_READY);
  ExpectRoundRobinPicks(picker.get(), {kFirstAddress, kSecondAddress});
  ExpectNoStateChange();

  // First channel removed, third added and made ready. First channel should
  // not show up.
  status = ApplyUpdate(BuildUpdateArgs({kSecondAddress, kThirdAddress}),
                       policy.get());
  EXPECT_TRUE(status.ok()) << status;
  FindSubchannel(kThirdAddress, {})
      ->SetConnectivityState(GRPC_CHANNEL_READY, {});
  ExpectState(GRPC_CHANNEL_READY);
  ExpectState(GRPC_CHANNEL_READY);
  picker = ExpectState(GRPC_CHANNEL_READY);
  ExpectRoundRobinPicks(picker.get(), {kSecondAddress, kThirdAddress});
  ExpectNoStateChange();
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
