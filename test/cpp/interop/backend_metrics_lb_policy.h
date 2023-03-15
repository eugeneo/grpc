//
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
//

#ifndef GRPC_TEST_CPP_INTEROP_BACKEND_METRICS_LB_POLICY_H
#define GRPC_TEST_CPP_INTEROP_BACKEND_METRICS_LB_POLICY_H

#include <grpc/support/port_platform.h>

#include <grpcpp/support/channel_arguments.h>

#include "src/core/lib/config/core_configuration.h"
#include "src/proto/grpc/testing/xds/v3/orca_load_report.pb.h"

namespace grpc {
namespace testing {
class LoadReportTracker {
 public:
  // A load report, or nullopt if the call had no load report.
  using LoadReportEntry = absl::optional<xds::data::orca::v3::OrcaLoadReport>;

  void RecordPerRpcLoadReport(
      const grpc_core::BackendMetricData* backend_metric_data);
  void RecordOobLoadReport(const grpc_core::BackendMetricData& oob_metric_data);
  // Returns the next per-RPC load report, or nullopt if the queue is empty.
  absl::optional<LoadReportEntry> GetNextLoadReport();
  LoadReportEntry WaitForOobLoadReport(
      const std::function<bool(const xds::data::orca::v3::OrcaLoadReport&)>&
          predicate,
      absl::Duration poll_timeout, size_t max_attempts);

 private:
  std::deque<LoadReportEntry> per_rpc_load_reports_
      ABSL_GUARDED_BY(load_reports_mu_);
  std::deque<xds::data::orca::v3::OrcaLoadReport> oob_load_reports_
      ABSL_GUARDED_BY(load_reports_mu_);
  absl::Mutex load_reports_mu_;
};

void RegisterBackendMetricsLbPolicy(
    grpc_core::CoreConfiguration::Builder* builder,
    std::shared_ptr<LoadReportTracker> load_report_tracker);
}  // namespace testing
}  // namespace grpc

#endif  // GRPC_TEST_CPP_INTEROP_BACKEND_METRICS_LB_POLICY_H