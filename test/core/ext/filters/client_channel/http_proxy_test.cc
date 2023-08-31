//
// Copyright 2023 gRPC authors.
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

#include "src/core/ext/filters/client_channel/http_proxy.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gtest/gtest.h"

#include <grpc/grpc.h>

#include "src/core/lib/address_utils/parse_address.h"
#include "test/core/util/scoped_env_var.h"
#include "test/core/util/test_config.h"

namespace grpc {
namespace testing {
namespace {

TEST(HttpProxyTest, ProxyDisabled) {
  auto initial = grpc_core::StringToSockaddr("127.0.0.1:3000");
  ASSERT_TRUE(initial.ok()) << initial.status().message();
  grpc_core::ChannelArgs args;
  grpc_core::HttpProxyMapper mapper;
  auto resolved = mapper.MapAddress(*initial, &args);
  EXPECT_FALSE(resolved.has_value());
}

TEST(HttpProxyTest, ProxyEnabled) {
  grpc_core::testing::ScopedEnvVar proxy_var("grpc_proxy", "127.0.0.5:3000");
  auto initial = grpc_core::StringToSockaddr("127.0.0.1:3000");
  ASSERT_TRUE(initial.ok()) << initial.status().message();
  grpc_core::ChannelArgs args;
  grpc_core::HttpProxyMapper mapper;
  auto resolved = mapper.MapAddress(*initial, &args);
  EXPECT_FALSE(resolved.has_value());
}

TEST(HttpProxyTests, DISABLED_ProxyEnabledViaArg) {}

}  // namespace
}  // namespace testing
}  // namespace grpc

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc_init();
  int ret = RUN_ALL_TESTS();
  grpc_shutdown();
  return ret;
}
