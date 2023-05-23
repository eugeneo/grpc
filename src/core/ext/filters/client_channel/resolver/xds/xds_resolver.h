//
// Copyright 2019 gRPC authors.
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

#ifndef GRPC_SRC_CORE_EXT_FILTERS_CLIENT_CHANNEL_RESOLVER_XDS_XDS_RESOLVER_H
#define GRPC_SRC_CORE_EXT_FILTERS_CLIENT_CHANNEL_RESOLVER_XDS_XDS_RESOLVER_H

#include <grpc/support/port_platform.h>

#include "absl/strings/string_view.h"

#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/gprpp/unique_type_name.h"
#include "src/core/lib/service_config/service_config_call_data.h"

namespace grpc_core {

class XdsClusterAttribute
    : public ServiceConfigCallData::CallAttributeInterface {
 public:
  static UniqueTypeName TypeName();

  explicit XdsClusterAttribute(absl::string_view cluster) : cluster_(cluster) {}

  absl::string_view cluster() const { return cluster_; }
  void set_cluster(absl::string_view cluster) { cluster_ = cluster; }

 private:
  UniqueTypeName type() const override { return TypeName(); }

  absl::string_view cluster_;
};

class ClusterRef;
class RouteData;

class XdsClusterDataAttribute
    : public ServiceConfigCallData::CallAttributeInterface {
 public:
  static UniqueTypeName TypeName();

  explicit XdsClusterDataAttribute(RefCountedPtr<RouteData> route_data,
                                   void* route);

  // This method can be called only once. The first call will release the
  // reference to the cluster map, and subsequent calls will return nullptr.
  RefCountedPtr<ClusterRef> LockAndGetCluster(absl::string_view cluster_name);

  bool HasClusterForRoute(absl::string_view cluster_name) const;

  UniqueTypeName type() const override { return TypeName(); }

 private:
  RefCountedPtr<RouteData> route_data_;
  // No need to leak another type
  void* opaque_route_;
};
}  // namespace grpc_core

#endif  // GRPC_SRC_CORE_EXT_FILTERS_CLIENT_CHANNEL_RESOLVER_XDS_XDS_RESOLVER_H
