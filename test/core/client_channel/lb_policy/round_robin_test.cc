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
 protected:
  RoundRobinTest() : policy_(MakeLbPolicy("round_robin")) {}

  // Picker should return each address in any order.
  void ExpectRoundRobinPicks(LoadBalancingPolicy::SubchannelPicker* picker,
                             absl::Span<const absl::string_view> uris,
                             size_t iterations_per_uri = 3,
                             SourceLocation location = SourceLocation()) {
    absl::optional<size_t> expected;
    for (size_t i = 0; i < iterations_per_uri * uris.size(); ++i) {
      auto address = ExpectPickComplete(picker);
      ASSERT_TRUE(address.has_value())
          << location.file() << ":" << location.line();
      auto index = std::find(uris.begin(), uris.end(), *address) - uris.begin();
      ASSERT_LT(index, uris.size())
          << "Missing " << *address << "\n"
          << location.file() << ":" << location.line();
      if (expected.has_value()) {
        EXPECT_EQ(index, *expected)
            << "Got " << *address << ", expected " << uris[index] << "\n"
            << location.file() << ":" << location.line();
      }
      expected = (index + 1) % uris.size();
    }
  }

  OrphanablePtr<LoadBalancingPolicy> policy_;
};

constexpr absl::string_view kFirstAddress = "ipv4:127.0.0.1:441";
constexpr absl::string_view kSecondAddress = "ipv4:127.0.0.1:442";
constexpr absl::string_view kThirdAddress = "ipv4:127.0.0.1:443";

TEST_F(RoundRobinTest, SingleAddress) {
  auto status = ApplyUpdate(BuildUpdate({kFirstAddress}), policy_.get());
  ASSERT_TRUE(status.ok()) << status;
  // LB policy should have reported CONNECTING state.
  ExpectConnectingUpdate();
  auto subchannel = FindSubchannel(kFirstAddress);
  ASSERT_NE(subchannel, nullptr);
  // LB policy should have requested a connection on this subchannel.
  EXPECT_TRUE(subchannel->ConnectionRequested());
  subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  ExpectConnectingUpdate();
  // Subchannel is ready
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
  auto picker = WaitForConnected();
  // Picker should return the same subchannel repeatedly.
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(ExpectPickComplete(picker.get()), kFirstAddress);
  }
  subchannel->SetConnectivityState(GRPC_CHANNEL_IDLE);
  ExpectReresolutionRequest();
  ExpectConnectingUpdate();
  subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  ExpectConnectingUpdate();
  // There's a failure
  subchannel->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                   absl::UnavailableError("a test"));
  ExpectReresolutionRequest();
  auto expected_status = absl::UnavailableError(
      "connections to all backends failing; "
      "last error: UNAVAILABLE: a test");
  WaitForConnectionFailedWithStatus(expected_status);
  subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  WaitForConnectionFailedWithStatus(expected_status);
  // ... and a recovery!
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
  picker = WaitForConnected();
  EXPECT_EQ(ExpectPickComplete(picker.get()), kFirstAddress);
}

TEST_F(RoundRobinTest, ThreeAddresses) {
  auto status = ApplyUpdate(BuildUpdate({
                                kFirstAddress,
                                kSecondAddress,
                                kThirdAddress,
                            }),
                            policy_.get());
  ASSERT_TRUE(status.ok()) << status;
  ExpectConnectingUpdate();
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
  for (size_t i = 0; i < subchannels.size(); i++) {
    subchannels[i]->SetConnectivityState(GRPC_CHANNEL_READY);
    // For the first subchannel, we use WaitForConnected() to drain any queued
    // CONNECTING updates.  For each successive subchannel, we can read just
    // one READY update at a time.
    auto picker = WaitForConnected();
    ASSERT_NE(picker, nullptr);
    ExpectRoundRobinPicks(picker.get(),
                          absl::Span<const absl::string_view>(
                              {kFirstAddress, kSecondAddress, kThirdAddress})
                              .subspan(0, i + 1));
  }
  subchannels[1]->SetConnectivityState(GRPC_CHANNEL_IDLE);
  ExpectReresolutionRequest();
  auto picker = WaitForConnected();
  ExpectRoundRobinPicks(picker.get(), {kFirstAddress, kThirdAddress});
  subchannels[1]->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  picker = WaitForConnected();
  subchannels[1]->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                       absl::UnknownError("This is a test"));
  ExpectReresolutionRequest();
  picker = WaitForConnected();
  ExpectRoundRobinPicks(picker.get(), {kFirstAddress, kThirdAddress});
}

TEST_F(RoundRobinTest, OneChannelReady) {
  auto subchannel = CreateSubchannel(kFirstAddress);
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
  auto status = ApplyUpdate(BuildUpdate({
                                kFirstAddress,
                                kSecondAddress,
                                kThirdAddress,
                            }),
                            policy_.get());
  ASSERT_TRUE(status.ok()) << status;
  ExpectConnectingUpdate();
  WaitForConnected();
  WaitForConnected();
  auto picker = WaitForConnected();
  ExpectRoundRobinPicks(picker.get(), {kFirstAddress});
}

TEST_F(RoundRobinTest, AllTransientFailure) {
  auto status = ApplyUpdate(BuildUpdate({
                                kFirstAddress,
                                kSecondAddress,
                                kThirdAddress,
                            }),
                            policy_.get());
  ASSERT_TRUE(status.ok()) << status;
  ExpectConnectingUpdate();
  for (auto address : {kFirstAddress, kSecondAddress}) {
    auto subchannel = FindSubchannel(address);
    ASSERT_NE(subchannel, nullptr);
    subchannel->SetConnectivityState(GRPC_CHANNEL_IDLE);
    subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
    subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
    WaitForConnected();
    subchannel->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                     absl::UnknownError("error1"));
    ExpectReresolutionRequest();
    ExpectConnectingUpdate();
  }
  auto third_subchannel = FindSubchannel(kThirdAddress);
  ASSERT_NE(third_subchannel, nullptr);
  third_subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
  WaitForConnected();
  third_subchannel->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                         absl::UnknownError("error2"));
  ExpectReresolutionRequest();
  WaitForConnectionFailedWithStatus(
      absl::UnavailableError("connections to all backends failing; "
                             "last error: UNKNOWN: error2"));
}

TEST_F(RoundRobinTest, EmptyAddressList) {
  LoadBalancingPolicy::UpdateArgs update_args;
  update_args.resolution_note = "This is a test";
  update_args.addresses.emplace();
  absl::Status status = ApplyUpdate(std::move(update_args), policy_.get());
  EXPECT_EQ(status,
            absl::UnavailableError("empty address list: This is a test"));
  WaitForConnectionFailedWithStatus(
      absl::UnavailableError("empty address list: This is a test"));
  // Fixes memory leaks. Will debug at a later point.
  EXPECT_TRUE(ApplyUpdate(BuildUpdate({kFirstAddress}), policy_.get()).ok());
  ExpectConnectingUpdate();
}

TEST_F(RoundRobinTest, AddressListChange) {
  std::array<SubchannelState*, 3> subchannels = {
      CreateSubchannel(kFirstAddress),
      CreateSubchannel(kSecondAddress),
      CreateSubchannel(kThirdAddress),
  };
  for (auto subchannel : subchannels) {
    subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  }

  // A subchannel that is ready. Gets picked all the time.
  auto status = ApplyUpdate(BuildUpdate({kFirstAddress}), policy_.get());
  ASSERT_TRUE(status.ok()) << status;
  ExpectConnectingUpdate();
  subchannels[0]->SetConnectivityState(GRPC_CHANNEL_READY);
  auto picker = WaitForConnected();
  auto picked = ExpectPickComplete(picker.get());
  EXPECT_EQ(*picked, kFirstAddress);
  // A second subchannel added, connecting. Only the first subchannel gets
  // picked still
  status =
      ApplyUpdate(BuildUpdate({kFirstAddress, kSecondAddress}), policy_.get());
  ASSERT_TRUE(status.ok()) << status;
  WaitForConnected();
  picker = WaitForConnected();
  EXPECT_EQ(*ExpectPickComplete(picker.get()), kFirstAddress);
  EXPECT_EQ(*ExpectPickComplete(picker.get()), kFirstAddress);
  // Second subchannel ready. Both subchannels are now picked.
  subchannels[1]->SetConnectivityState(GRPC_CHANNEL_READY);
  picker = WaitForConnected();
  ExpectRoundRobinPicks(picker.get(), {kFirstAddress, kSecondAddress});
  // First address removed, third added and made ready. First subchannel should
  // not show up.
  status =
      ApplyUpdate(BuildUpdate({kSecondAddress, kThirdAddress}), policy_.get());
  EXPECT_TRUE(status.ok()) << status;
  subchannels[2]->SetConnectivityState(GRPC_CHANNEL_READY, {});
  WaitForConnected();
  WaitForConnected();
  picker = WaitForConnected();
  ExpectRoundRobinPicks(picker.get(), {kSecondAddress, kThirdAddress});
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
