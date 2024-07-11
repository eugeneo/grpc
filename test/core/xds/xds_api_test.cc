//
// Copyright 2024 gRPC authors.
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

#include "src/core/xds/xds_client/xds_api.h"

#include <gtest/gtest.h>

#include <grpc/grpc.h>

#include "src/proto/grpc/testing/xds/v3/lrs.pb.h"
#include "test/core/test_util/test_config.h"

namespace grpc_core {
namespace testing {
namespace {

TEST(XdsApiTest, CreateLrsRequestDoesNotCrash) {
  upb::DefPool pool;
  XdsApi api(nullptr, &xds_client_trace, nullptr, &pool, "user_agent_name",
             "user_agent_version");
  XdsApi::ClusterLoadReportMap map;
  map[{"cluster_name", "eds_service"}] = XdsApi::ClusterLoadReport();
  envoy::service::load_stats::v3::LoadStatsRequest req;
  req.ParseFromString(api.CreateLrsRequest(map));
  EXPECT_EQ(req.DebugString(),
            "cluster_stats {\n  cluster_name: \"cluster_name\"\n  "
            "load_report_interval {\n  }\n  cluster_service_name: "
            "\"eds_service\"\n}\n");
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
