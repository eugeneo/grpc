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
#include <iterator>
#include <set>
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
      bool shuffleAddressList) {
    return MakeConfig(Json::FromArray({Json::FromObject({{
        "pick_first",
        Json::FromObject(
            {{"shuffleAddressList", Json::FromBool(shuffleAddressList)}}),
    }})}));
  }

  template <typename C>
  absl::string_view GetConnectionRequestedAddress(
      const C& addresses, bool shuffle,
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
    absl::c_remove_copy_if(addresses, std::back_inserter(filtered),
                           [=](auto addr) { return addr == address; });
    return filtered;
  }

  std::set<absl::string_view> ResetPickedAddress(
      absl::Span<const absl::string_view> addresses, size_t iterations,
      bool shuffle) {
    std::set<absl::string_view> selected_addresses;
    absl::string_view address_to_ignore = "";
    for (size_t i = 0; i < iterations; i++) {
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

TEST_F(PickFirstTest, Basic) {
  constexpr std::array<absl::string_view, 3> kAddressUris = {
      "ipv4:127.0.0.1:443", "ipv4:127.0.0.1:444", "ipv4:127.0.0.1:445"};
  // Send an update containing one address.
  absl::Status status =
      ApplyUpdate(BuildUpdate(kAddressUris), lb_policy_.get());
  EXPECT_TRUE(status.ok()) << status;
  // LB policy should have created a subchannel for the address with the
  // GRPC_ARG_INHIBIT_HEALTH_CHECKING channel arg.
  auto* subchannel =
      FindSubchannel(kAddressUris[0],
                     ChannelArgs().Set(GRPC_ARG_INHIBIT_HEALTH_CHECKING, true));
  ASSERT_NE(subchannel, nullptr);
  // When the LB policy receives the subchannel's initial connectivity
  // state notification (IDLE), it will request a connection.
  EXPECT_TRUE(subchannel->ConnectionRequested());
  // This causes the subchannel to start to connect, so it reports CONNECTING.
  subchannel->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  // LB policy should have reported CONNECTING state.
  ExpectConnectingUpdate();
  // When the subchannel becomes connected, it reports READY.
  subchannel->SetConnectivityState(GRPC_CHANNEL_READY);
  // The LB policy will report CONNECTING some number of times (doesn't
  // matter how many) and then report READY.
  auto picker = WaitForConnected();
  ASSERT_NE(picker, nullptr);
  // Picker should return the same subchannel repeatedly.
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(ExpectPickComplete(picker.get()), kAddressUris[0]);
  }
}

TEST_F(PickFirstTest, WithShuffle) {
  constexpr size_t kPolicyUpdates = 20;
  constexpr std::array<absl::string_view, 6> kAddressUris = {
      "ipv4:127.0.0.1:443", "ipv4:127.0.0.1:444", "ipv4:127.0.0.1:445",
      "ipv4:127.0.0.1:446", "ipv4:127.0.0.1:447", "ipv4:127.0.0.1:448"};
  // With shuffling, different addresses should be returned.
  EXPECT_GT(ResetPickedAddress(kAddressUris, kPolicyUpdates, true).size(), 1);
  // Without shuffling, the same address will always be returned
  EXPECT_EQ(ResetPickedAddress(kAddressUris, kPolicyUpdates, false).size(), 1);
}

}  // namespace
}  // namespace testing
}  // namespace grpc_core

int main(int argc, char** argv) {
  grpc_core::testing::ScopedExperimentalEnvVar env_var(
      "GRPC_EXPERIMENTAL_PICKFIRST_LB_CONFIG");
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc_init();
  int ret = RUN_ALL_TESTS();
  grpc_shutdown();
  return ret;
}
