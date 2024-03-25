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
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/cleanup/cleanup.h"
#include "absl/strings/str_format.h"

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/status.h>

#include "src/core/client_channel/backup_poller.h"
#include "src/core/lib/config/config_vars.h"
#include "src/core/lib/gprpp/env.h"
#include "src/cpp/client/secure_credentials.h"
#include "src/proto/grpc/testing/echo.grpc.pb.h"
#include "src/proto/grpc/testing/echo_messages.pb.h"
#include "src/proto/grpc/testing/xds/v3/cluster.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/endpoint.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/http_connection_manager.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/listener.grpc.pb.h"
#include "src/proto/grpc/testing/xds/v3/route.grpc.pb.h"
#include "test/core/util/resolve_localhost_ip46.h"
#include "test/core/util/scoped_env_var.h"
#include "test/core/util/test_config.h"
#include "test/cpp/end2end/xds/xds_end2end_test_lib.h"
#include "test/cpp/end2end/xds/xds_utils.h"

namespace grpc {
namespace testing {
namespace {

constexpr char const* kErrorMessage = "test forced ADS stream failure";

class XdsFallbackTest : public XdsEnd2endTest {
 public:
  XdsFallbackTest()
      : fallback_balancer_(CreateAndStartBalancer("Fallback Balancer")) {}

  void SetUp() override {
    // Overrides SetUp from a base class so we can call InitClient per-test case
  }

  void TearDown() override {
    fallback_balancer_->Shutdown();
    XdsEnd2endTest::TearDown();
  }

  void SetXdsResourcesForTarget(BalancerServerThread* balancer, size_t backend,
                                absl::string_view target = "") {
    Listener listener = default_listener_;
    RouteConfiguration route_config = default_route_config_;
    Cluster cluster = default_cluster_;
    // Target 0 uses default resources when no authority, to enable using more
    // test framework functions
    if (!target.empty()) {
      listener.set_name(target);
      cluster.set_name(absl::StrFormat("cluster_%s", target));
      cluster.mutable_eds_cluster_config()->set_service_name(
          absl::StrFormat("eds_%s", target));
      route_config.set_name(absl::StrFormat("route_%s", target));
      route_config.mutable_virtual_hosts(0)
          ->mutable_routes(0)
          ->mutable_route()
          ->set_cluster(cluster.name());
    }
    SetListenerAndRouteConfiguration(balancer, listener, route_config);
    balancer->ads_service()->SetCdsResource(cluster);
    balancer->ads_service()->SetEdsResource(BuildEdsResource(
        EdsResourceArgs(
            {{"locality0", CreateEndpointsForBackends(backend, backend + 1)}}),
        cluster.eds_cluster_config().service_name()));
  }

  void ConfigureAuthority(BalancerServerThread* balancer,
                          absl::string_view authority,
                          absl::string_view server_name, int backend) {
    std::string listener_name = absl::StrFormat(
        "xdstp://%s/envoy.config.listener.v3.Listener/"
        "whee%%25/%s",
        authority, server_name);
    std::string cluster_name = absl::StrFormat(
        "xdstp://%s/envoy.config.cluster.v3.Cluster/cluster_name_%d", authority,
        backend);
    std::string route_config_name = absl::StrFormat(
        "xdstp://%s/envoy.config.route.v3.RouteConfiguration/"
        "new_route_config_name_%d",
        authority, backend);
    std::string eds_server_name = absl::StrFormat(
        "xdstp://%s/envoy.config.endpoint.v3.ClusterLoadAssignment/eds_%d",
        authority, backend);
    balancer->ads_service()->SetEdsResource(BuildEdsResource(
        EdsResourceArgs(
            {{"locality0", CreateEndpointsForBackends(backend, backend + 1)}}),
        eds_server_name));
    // New cluster
    Cluster new_cluster = default_cluster_;
    new_cluster.set_name(cluster_name);
    new_cluster.mutable_eds_cluster_config()->set_service_name(eds_server_name);
    balancer->ads_service()->SetCdsResource(new_cluster);
    // New Route
    RouteConfiguration new_route_config = default_route_config_;
    new_route_config.set_name(route_config_name);
    new_route_config.mutable_virtual_hosts(0)
        ->mutable_routes(0)
        ->mutable_route()
        ->set_cluster(cluster_name);
    // New Listener
    Listener listener = default_listener_;
    listener.set_name(listener_name);
    SetListenerAndRouteConfiguration(balancer, listener, new_route_config);
  }

  void ExpectBackendCall(EchoTestService::Stub* stub, int backend,
                         grpc_core::DebugLocation location) {
    ClientContext context;
    EchoRequest request;
    EchoResponse response;
    RpcOptions().SetupRpc(&context, &request);
    Status status = stub->Echo(&context, request, &response);
    EXPECT_TRUE(status.ok()) << "code=" << status.error_code()
                             << " message=" << status.error_message() << "\n"
                             << location.file() << ':' << location.line();
    EXPECT_EQ(1U, backends_[backend]->backend_service()->request_count())
        << "\n"
        << location.file() << ':' << location.line();
  }

 protected:
  std::unique_ptr<BalancerServerThread> fallback_balancer_;
};

TEST_P(XdsFallbackTest, FallbackAndRecover) {
  grpc_core::testing::ScopedEnvVar fallback_enabled(
      "GRPC_EXPERIMENTAL_XDS_FALLBACK", "1");
  auto broken_balancer = CreateAndStartBalancer("Broken balancer");
  broken_balancer->ads_service()->ForceADSFailure(
      Status(StatusCode::RESOURCE_EXHAUSTED, kErrorMessage));
  InitClient(XdsBootstrapBuilder().SetServers({
      balancer_->address(),
      broken_balancer->address(),
      fallback_balancer_->address(),
  }));
  // Primary xDS server has backends_[0] configured and fallback server has
  // backends_[1]
  CreateAndStartBackends(2);
  SetXdsResourcesForTarget(balancer_.get(), 0);
  SetXdsResourcesForTarget(fallback_balancer_.get(), 1);
  balancer_->ads_service()->ForceADSFailure(
      Status(StatusCode::RESOURCE_EXHAUSTED, kErrorMessage));
  // Primary server down, fallback server data is used (backends_[1])
  CheckRpcSendOk(DEBUG_LOCATION);
  EXPECT_EQ(backends_[0]->backend_service()->request_count(), 0);
  EXPECT_EQ(backends_[1]->backend_service()->request_count(), 1);
  // Primary server is back. backends_[0] will be used when the data makes it
  // all way to the client
  balancer_->ads_service()->ClearADSFailure();
  WaitForBackend(DEBUG_LOCATION, 0);
  broken_balancer->Shutdown();
}

TEST_P(XdsFallbackTest, EnvVarNotSet) {
  InitClient(XdsBootstrapBuilder().SetServers({
      balancer_->address(),
      fallback_balancer_->address(),
  }));
  // Primary xDS server has backends_[0] configured and fallback server has
  // backends_[1]
  CreateAndStartBackends(2);
  SetXdsResourcesForTarget(balancer_.get(), 0);
  SetXdsResourcesForTarget(fallback_balancer_.get(), 1);
  balancer_->ads_service()->ForceADSFailure(
      Status(StatusCode::RESOURCE_EXHAUSTED, kErrorMessage));
  // Primary server down, failure should be reported
  CheckRpcSendFailure(
      DEBUG_LOCATION, StatusCode::UNAVAILABLE,
      absl::StrFormat("server.example.com: UNAVAILABLE: xDS channel for server "
                      "localhost:%d: xDS call failed with no responses "
                      "received; status: RESOURCE_EXHAUSTED: test forced ADS "
                      "stream failure \\(node ID:xds_end2end_test\\)",
                      balancer_->port()));
}

TEST_P(XdsFallbackTest, PrimarySecondaryNotAvailable) {
  grpc_core::testing::ScopedEnvVar fallback_enabled(
      "GRPC_EXPERIMENTAL_XDS_FALLBACK", "1");
  InitClient(XdsBootstrapBuilder().SetServers(
      {balancer_->address(), fallback_balancer_->address()}));
  balancer_->ads_service()->ForceADSFailure(
      Status(StatusCode::RESOURCE_EXHAUSTED, kErrorMessage));
  fallback_balancer_->ads_service()->ForceADSFailure(
      Status(StatusCode::RESOURCE_EXHAUSTED, kErrorMessage));
  CheckRpcSendFailure(
      DEBUG_LOCATION, StatusCode::UNAVAILABLE,
      absl::StrFormat(
          "server.example.com: UNAVAILABLE: xDS channel for server "
          "localhost:%d: xDS call failed with no responses received; "
          "status: RESOURCE_EXHAUSTED: test forced ADS stream failure \\(node "
          "ID:xds_end2end_test\\)",
          fallback_balancer_->port()));
}

TEST_P(XdsFallbackTest, UsesCachedResourcesAfterFailure) {
  constexpr absl::string_view kServerName2 = "server2.example.com";
  grpc_core::testing::ScopedEnvVar fallback_enabled(
      "GRPC_EXPERIMENTAL_XDS_FALLBACK", "1");
  InitClient(XdsBootstrapBuilder().SetServers(
      {balancer_->address(), fallback_balancer_->address()}));
  // 4 backends - cross product of two data plane targets and two balancers
  CreateAndStartBackends(4);
  SetXdsResourcesForTarget(balancer_.get(), 0);
  SetXdsResourcesForTarget(fallback_balancer_.get(), 1);
  SetXdsResourcesForTarget(balancer_.get(), 2, kServerName2);
  SetXdsResourcesForTarget(fallback_balancer_.get(), 3, kServerName2);
  CheckRpcSendOk(DEBUG_LOCATION);
  EXPECT_EQ(backends_[0]->backend_service()->request_count(), 1);
  balancer_->ads_service()->ForceADSFailure(
      Status(StatusCode::RESOURCE_EXHAUSTED, kErrorMessage));
  auto channel = CreateChannel(0, std::string(kServerName2).c_str());
  auto stub = grpc::testing::EchoTestService::NewStub(channel);
  // server2.example.com is configured from the fallback server
  ExpectBackendCall(stub.get(), 3, DEBUG_LOCATION);
  // Calling server.example.com still uses cached value
  CheckRpcSendOk(DEBUG_LOCATION);
  EXPECT_EQ(backends_[0]->backend_service()->request_count(), 2);
  EXPECT_EQ(backends_[1]->backend_service()->request_count(), 0);
}

TEST_P(XdsFallbackTest, PerAuthorityFallback) {
  auto fallback_balancer2 = CreateAndStartBalancer("Fallback for Authority2");
  // Use cleanup in case test assertion fails
  auto balancer2_cleanup =
      absl::MakeCleanup([&]() { fallback_balancer2->Shutdown(); });
  grpc_core::testing::ScopedEnvVar fallback_enabled(
      "GRPC_EXPERIMENTAL_XDS_FALLBACK", "1");
  grpc_core::testing::ScopedExperimentalEnvVar env_var(
      "GRPC_EXPERIMENTAL_XDS_FEDERATION");
  const char* kAuthority1 = "xds1.example.com";
  const char* kAuthority2 = "xds2.example.com";
  constexpr absl::string_view kServer1Name = "server1.example.com";
  constexpr absl::string_view kServer2Name = "server2.example.com";
  // Authority1 uses balancer_ and fallback_balancer_
  // Authority2 uses balancer_ and fallback_balancer2
  XdsBootstrapBuilder builder;
  builder.SetServers({balancer_->address()});
  builder.AddAuthority(kAuthority1,
                       {balancer_->address(), fallback_balancer_->address()});
  builder.AddAuthority(kAuthority2,
                       {balancer_->address(), fallback_balancer2->address()});
  InitClient(builder);
  CreateAndStartBackends(4);
  ConfigureAuthority(fallback_balancer_.get(), kAuthority1, kServer1Name, 0);
  ConfigureAuthority(fallback_balancer2.get(), kAuthority2, kServer2Name, 1);
  ConfigureAuthority(balancer_.get(), kAuthority1, kServer1Name, 2);
  ConfigureAuthority(balancer_.get(), kAuthority2, kServer2Name, 3);
  // Primary balancer is down, using the fallback servers
  balancer_->ads_service()->ForceADSFailure(
      Status(StatusCode::RESOURCE_EXHAUSTED, kErrorMessage));
  // Create second channel to new target URI and send 1 RPC.
  auto authority1_stub = grpc::testing::EchoTestService::NewStub(CreateChannel(
      /*failover_timeout_ms=*/0,
      absl::StrFormat("whee%%/%s", kServer1Name).c_str(), kAuthority1));
  auto authority2_stub = grpc::testing::EchoTestService::NewStub(CreateChannel(
      /*failover_timeout_ms=*/0,
      absl::StrFormat("whee%%/%s", kServer2Name).c_str(), kAuthority2));
  ExpectBackendCall(authority1_stub.get(), 0, DEBUG_LOCATION);
  ExpectBackendCall(authority2_stub.get(), 1, DEBUG_LOCATION);
  // Primary balancer is up, its data will be used now.
  balancer_->ads_service()->ClearADSFailure();
  auto deadline = absl::Now() + absl::Seconds(5);
  while (absl::Now() < deadline &&
         (backends_[2]->backend_service()->request_count() == 0 ||
          backends_[3]->backend_service()->request_count() == 0)) {
    ClientContext context;
    EchoRequest request;
    EchoResponse response;
    RpcOptions().SetupRpc(&context, &request);
    Status status = authority1_stub->Echo(&context, request, &response);
    ClientContext context2;
    EchoRequest request2;
    EchoResponse response2;
    RpcOptions().SetupRpc(&context2, &request2);
    // RPCs may briefly fail if the config tear happens
    status = authority2_stub->Echo(&context2, request2, &response2);
  }
  ASSERT_LE(1U, backends_[2]->backend_service()->request_count());
  ASSERT_LE(1U, backends_[3]->backend_service()->request_count());
}

TEST_P(XdsFallbackTest, DISABLED_FallbackAfterSetup) {
  grpc_core::testing::ScopedEnvVar fallback_enabled(
      "GRPC_EXPERIMENTAL_XDS_FALLBACK", "1");
}

INSTANTIATE_TEST_SUITE_P(XdsTest, XdsFallbackTest,
                         ::testing::Values(XdsTestType().set_bootstrap_source(
                             XdsTestType::kBootstrapFromEnvVar)),
                         &XdsTestType::Name);

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
  const auto result = RUN_ALL_TESTS();
  grpc_shutdown();
  return result;
}
