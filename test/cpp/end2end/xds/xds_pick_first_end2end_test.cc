// Copyright 2017 gRPC authors.
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

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

#include <grpc/event_engine/endpoint_config.h>

#include "src/core/ext/filters/client_channel/backup_poller.h"
#include "src/core/ext/filters/client_channel/lb_policy/xds/xds_channel_args.h"
#include "src/core/ext/filters/client_channel/resolver/fake/fake_resolver.h"
#include "src/core/lib/address_utils/sockaddr_utils.h"
#include "src/core/lib/config/config_vars.h"
#include "src/core/lib/gprpp/env.h"
#include "src/proto/grpc/testing/xds/v3/aggregate_cluster.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/cluster.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/pick_first.pb.h"
#include "test/cpp/end2end/connection_attempt_injector.h"
#include "test/cpp/end2end/xds/xds_end2end_test_lib.h"

namespace grpc {
namespace testing {
namespace {

using ::envoy::config::cluster::v3::CustomClusterType;
using ::envoy::config::core::v3::HealthStatus;
using ::envoy::extensions::clusters::aggregate::v3::ClusterConfig;
using ::envoy::extensions::load_balancing_policies::pick_first::v3::PickFirst;

class PickFirstTest : public XdsEnd2endTest {
 protected:
  void SetUp() override {
    logical_dns_cluster_resolver_response_generator_ =
        grpc_core::MakeRefCounted<grpc_core::FakeResolverResponseGenerator>();
    InitClient();
    SetUpChannel();
  }

  void SetUpChannel(ChannelArguments* args = nullptr) {
    ChannelArguments local_args;
    if (args == nullptr) args = &local_args;
    args->SetPointerWithVtable(
        GRPC_ARG_XDS_LOGICAL_DNS_CLUSTER_FAKE_RESOLVER_RESPONSE_GENERATOR,
        logical_dns_cluster_resolver_response_generator_.get(),
        &grpc_core::FakeResolverResponseGenerator::kChannelArgPointerVtable);
    ResetStub(/*failover_timeout_ms=*/0, args);
  }

  grpc_core::ServerAddressList CreateAddressListFromPortList(
      const std::vector<int>& ports) {
    grpc_core::ServerAddressList addresses;
    for (int port : ports) {
      absl::StatusOr<grpc_core::URI> lb_uri = grpc_core::URI::Parse(
          absl::StrCat(ipv6_only_ ? "ipv6:[::1]:" : "ipv4:127.0.0.1:", port));
      GPR_ASSERT(lb_uri.ok());
      grpc_resolved_address address;
      GPR_ASSERT(grpc_parse_uri(*lb_uri, &address));
      addresses.emplace_back(address, grpc_core::ChannelArgs());
    }
    return addresses;
  }

  std::string CreateMetadataValueThatHashesToBackendPort(int port) {
    return absl::StrCat(ipv6_only_ ? "[::1]" : "127.0.0.1", ":", port, "_0");
  }

  std::string CreateMetadataValueThatHashesToBackend(int index) {
    return CreateMetadataValueThatHashesToBackendPort(backends_[index]->port());
  }

  grpc_core::RefCountedPtr<grpc_core::FakeResolverResponseGenerator>
      logical_dns_cluster_resolver_response_generator_;
};

// Run both with and without load reporting, just for test coverage.
INSTANTIATE_TEST_SUITE_P(
    XdsTest, PickFirstTest,
    ::testing::Values(XdsTestType(), XdsTestType().set_enable_load_reporting()),
    &XdsTestType::Name);

TEST_P(PickFirstTest, WorksWithoutShuffling) {
  CreateAndStartBackends(2);
  const char* kNewCluster1Name = "new_cluster_1";
  const char* kNewEdsService1Name = "new_eds_service_name_1";
  const char* kNewCluster2Name = "new_cluster_2";
  const char* kNewEdsService2Name = "new_eds_service_name_2";
  // Populate new EDS resources.
  EdsResourceArgs args1({
      {"locality0", {MakeNonExistantEndpoint(), MakeNonExistantEndpoint()}},
  });
  EdsResourceArgs args2({
      {"locality0", CreateEndpointsForBackends()},
  });
  balancer_->ads_service()->SetEdsResource(
      BuildEdsResource(args1, kNewEdsService1Name));
  balancer_->ads_service()->SetEdsResource(
      BuildEdsResource(args2, kNewEdsService2Name));
  // Populate new CDS resources.
  Cluster new_cluster1 = default_cluster_;
  new_cluster1.set_name(kNewCluster1Name);
  new_cluster1.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService1Name);
  balancer_->ads_service()->SetCdsResource(new_cluster1);
  Cluster new_cluster2 = default_cluster_;
  new_cluster2.set_name(kNewCluster2Name);
  new_cluster2.mutable_eds_cluster_config()->set_service_name(
      kNewEdsService2Name);
  balancer_->ads_service()->SetCdsResource(new_cluster2);
  // Create Aggregate Cluster
  auto cluster = default_cluster_;
  cluster.mutable_load_balancing_policy()
      ->add_policies()
      ->mutable_typed_extension_config()
      ->mutable_typed_config()
      ->PackFrom(PickFirst());
  CustomClusterType* custom_cluster = cluster.mutable_cluster_type();
  custom_cluster->set_name("envoy.clusters.aggregate");
  ClusterConfig cluster_config;
  cluster_config.add_clusters(kNewCluster1Name);
  cluster_config.add_clusters(kNewCluster2Name);
  custom_cluster->mutable_typed_config()->PackFrom(cluster_config);
  balancer_->ads_service()->SetCdsResource(cluster);
  // Set up route with channel id hashing
  auto new_route_config = default_route_config_;
  auto* route = new_route_config.mutable_virtual_hosts(0)->mutable_routes(0);
  auto* hash_policy = route->mutable_route()->add_hash_policy();
  hash_policy->mutable_filter_state()->set_key("io.grpc.channel_id");
  SetListenerAndRouteConfiguration(balancer_.get(), default_listener_,
                                   new_route_config);
  // Verifying that we are using ring hash as only 1 endpoint is receiving all
  // the traffic.
  CheckRpcSendOk(DEBUG_LOCATION, 100, RpcOptions().set_timeout_ms(5000));
  bool found = false;
  for (size_t i = 0; i < backends_.size(); ++i) {
    if (backends_[i]->backend_service()->request_count() > 0) {
      EXPECT_EQ(backends_[i]->backend_service()->request_count(), 100)
          << "backend " << i;
      EXPECT_FALSE(found) << "backend " << i;
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

}  // namespace
}  // namespace testing
}  // namespace grpc

int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(&argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  // Make the backup poller poll very frequently in order to pick up
  // updates from all the subchannels's FDs.
  grpc_core::ConfigVars::Overrides overrides;
  overrides.client_channel_backup_poll_interval_ms = 1;
  grpc_core::ConfigVars::SetOverrides(overrides);
#if TARGET_OS_IPHONE
  // Workaround Apple CFStream bug
  grpc_core::SetEnv("grpc_cfstream", "0");
#endif
  grpc_init();
  grpc::testing::ConnectionAttemptInjector::Init();
  const auto result = RUN_ALL_TESTS();
  grpc_shutdown();
  return result;
}
