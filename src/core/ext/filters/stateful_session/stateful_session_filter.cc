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

#include <grpc/support/port_platform.h>

#include "src/core/ext/filters/stateful_session/stateful_session_filter.h"

#include <string.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"

#include <grpc/support/log.h>
#include <grpc/support/time.h>

#include "src/core/ext/filters/client_channel/resolver/xds/xds_resolver.h"
#include "src/core/ext/filters/stateful_session/stateful_session_service_config_parser.h"
#include "src/core/lib/channel/channel_stack.h"
#include "src/core/lib/channel/context.h"
#include "src/core/lib/config/core_configuration.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/gprpp/crash.h"
#include "src/core/lib/gprpp/time.h"
#include "src/core/lib/promise/context.h"
#include "src/core/lib/promise/map.h"
#include "src/core/lib/promise/pipe.h"
#include "src/core/lib/promise/poll.h"
#include "src/core/lib/resource_quota/arena.h"
#include "src/core/lib/service_config/service_config_call_data.h"
#include "src/core/lib/slice/slice.h"
#include "src/core/lib/transport/metadata_batch.h"
#include "src/core/lib/transport/transport.h"

namespace grpc_core {

TraceFlag grpc_stateful_session_filter_trace(false, "stateful_session_filter");

UniqueTypeName XdsOverrideHostAttribute::TypeName() {
  static UniqueTypeName::Factory kFactory("xds_override_host");
  return kFactory.Create();
}

const grpc_channel_filter StatefulSessionFilter::kFilter =
    MakePromiseBasedFilter<StatefulSessionFilter, FilterEndpoint::kClient,
                           kFilterExaminesServerInitialMetadata>(
        "stateful_session_filter");

absl::StatusOr<StatefulSessionFilter> StatefulSessionFilter::Create(
    const ChannelArgs&, ChannelFilter::Args filter_args) {
  return StatefulSessionFilter(filter_args);
}

StatefulSessionFilter::StatefulSessionFilter(ChannelFilter::Args filter_args)
    : index_(grpc_channel_stack_filter_instance_number(
          filter_args.channel_stack(),
          filter_args.uninitialized_channel_element())),
      service_config_parser_index_(
          StatefulSessionServiceConfigParser::ParserIndex()) {}

namespace {

absl::string_view ToArena(absl::string_view src) {
  if (src.empty()) {
    return src;
  }
  char* host_allocated_value =
      static_cast<char*>(GetContext<Arena>()->Alloc(src.size()));
  memcpy(host_allocated_value, src.data(), src.size());
  return absl::string_view(host_allocated_value, src.size());
}

// Adds the set-cookie header to the server initial metadata if needed.
void MaybeUpdateServerInitialMetadata(
    const StatefulSessionMethodParsedConfig::CookieConfig* cookie_config,
    absl::string_view cookie_value, absl::string_view actual_cluster,
    ServerMetadata* server_initial_metadata) {
  // Get peer string.
  Slice* peer_string = server_initial_metadata->get_pointer(PeerString());
  if (peer_string == nullptr) {
    // No changes, keep the same set-cookie header.
    return;
  }
  std::string new_value(peer_string->as_string_view());
  if (!actual_cluster.empty()) {
    absl::StrAppend(&new_value, ";", actual_cluster);
  }
  if (new_value == cookie_value) {
    return;
  }
  std::vector<std::string> parts = {absl::StrCat(
      *cookie_config->name, "=", absl::Base64Escape(new_value), "; HttpOnly")};
  if (!cookie_config->path.empty()) {
    parts.emplace_back(absl::StrCat("Path=", cookie_config->path));
  }
  if (cookie_config->ttl > Duration::Zero()) {
    parts.emplace_back(
        absl::StrCat("Max-Age=", cookie_config->ttl.as_timespec().tv_sec));
  }
  server_initial_metadata->Append(
      "set-cookie", Slice::FromCopiedString(absl::StrJoin(parts, "; ")),
      [](absl::string_view error, const Slice&) {
        Crash(absl::StrCat("ERROR ADDING set-cookie METADATA: ", error));
      });
}

absl::string_view GetClusterToUse(
    absl::string_view cluster_from_cookie,
    ServiceConfigCallData* service_config_call_data) {
  static constexpr absl::string_view kClusterPrefix = "cluster:";
  auto cluster_attribute =
      service_config_call_data->GetCallAttribute<XdsClusterAttribute>();
  GPR_ASSERT(cluster_attribute != nullptr);
  auto cluster = cluster_attribute->cluster();
  auto cluster_to_use = cluster;
  if (!absl::StartsWith(cluster, kClusterPrefix)) {
    return absl::string_view();
  }
  if (!cluster_from_cookie.empty()) {
    auto route_data =
        service_config_call_data->GetCallAttribute<XdsRouteStateAttribute>();
    GPR_ASSERT(route_data != nullptr);
    if (route_data->HasClusterForRoute(
            absl::StripPrefix(cluster_from_cookie, kClusterPrefix))) {
      // This string is already allocated on arena
      cluster_to_use = cluster_from_cookie;
    } else {
      cluster_to_use = ToArena(cluster);
    }
  }
  cluster_attribute->set_cluster(cluster_to_use);
  return cluster_to_use;
}

std::string GetCookieValue(const ClientMetadataHandle& client_initial_metadata,
                           absl::string_view cookie_name) {
  // Check to see if the cookie header is present.
  std::string buffer;
  auto header_value =
      client_initial_metadata->GetStringValue("cookie", &buffer);
  if (!header_value.has_value()) return "";
  // Parse cookie header.
  std::vector<absl::string_view> values;
  for (absl::string_view cookie : absl::StrSplit(*header_value, "; ")) {
    std::pair<absl::string_view, absl::string_view> kv =
        absl::StrSplit(cookie, absl::MaxSplits('=', 1));
    if (kv.first == cookie_name) values.push_back(kv.second);
  }
  if (values.empty()) return "";
  // TODO(roth): Figure out the right behavior for multiple cookies.
  // For now, just choose the first value.
  std::string decoded;
  if (absl::Base64Unescape(values.front(), &decoded)) {
    return decoded;
  }
  return "";
}

bool IsConfiguredPath(absl::string_view configured_path,
                      const ClientMetadataHandle& client_initial_metadata) {
  // No path configured meaning all paths match
  if (configured_path.empty()) {
    return true;
  }
  // Check to see if the configured path matches the request path.
  Slice* path_slice = client_initial_metadata->get_pointer(HttpPathMetadata());
  GPR_ASSERT(path_slice != nullptr);
  absl::string_view path = path_slice->as_string_view();
  // Matching criteria from
  // https://www.rfc-editor.org/rfc/rfc6265#section-5.1.4.
  // The cookie-path is a prefix of the request-path (and)
  if (!absl::StartsWith(path, configured_path)) {
    return false;
  }
  // One of
  // 1. The cookie-path and the request-path are identical.
  // 2. The last character of the cookie-path is %x2F ("/").
  // 3. The first character of the request-path that is not included
  //    in the cookie-path is a %x2F ("/") character.
  return path.length() == configured_path.length() ||
         configured_path.back() == '/' || path[configured_path.length()] == '/';
}
}  // namespace

// Construct a promise for one call.
ArenaPromise<ServerMetadataHandle> StatefulSessionFilter::MakeCallPromise(
    CallArgs call_args, NextPromiseFactory next_promise_factory) {
  // Get config.
  auto* service_config_call_data = static_cast<ServiceConfigCallData*>(
      GetContext<
          grpc_call_context_element>()[GRPC_CONTEXT_SERVICE_CONFIG_CALL_DATA]
          .value);
  GPR_ASSERT(service_config_call_data != nullptr);
  auto* method_params = static_cast<StatefulSessionMethodParsedConfig*>(
      service_config_call_data->GetMethodParsedConfig(
          service_config_parser_index_));
  GPR_ASSERT(method_params != nullptr);
  auto* cookie_config = method_params->GetConfig(index_);
  GPR_ASSERT(cookie_config != nullptr);
  if (!cookie_config->name.has_value() ||
      !IsConfiguredPath(cookie_config->path,
                        call_args.client_initial_metadata)) {
    return next_promise_factory(std::move(call_args));
  }
  // Base64-decode cookie value.
  absl::string_view cookie_value = ToArena(
      GetCookieValue(call_args.client_initial_metadata, *cookie_config->name));
  std::pair<absl::string_view, absl::string_view> host_cluster =
      absl::StrSplit(cookie_value, absl::MaxSplits(';', 1));
  if (!host_cluster.first.empty()) {
    service_config_call_data->SetCallAttribute(
        GetContext<Arena>()->New<XdsOverrideHostAttribute>(host_cluster.first));
  }
  absl::string_view cluster_name =
      GetClusterToUse(host_cluster.second, service_config_call_data);
  // Intercept server initial metadata.
  call_args.server_initial_metadata->InterceptAndMap(
      [=](ServerMetadataHandle md) {
        // Add cookie to server initial metadata if needed.
        MaybeUpdateServerInitialMetadata(cookie_config, cookie_value,
                                         cluster_name, md.get());
        return md;
      });
  return Map(next_promise_factory(std::move(call_args)),
             [=](ServerMetadataHandle md) {
               // If we got a Trailers-Only response, then add the
               // cookie to the trailing metadata instead of the
               // initial metadata.
               if (md->get(GrpcTrailersOnly()).value_or(false)) {
                 MaybeUpdateServerInitialMetadata(cookie_config, cookie_value,
                                                  cluster_name, md.get());
               }
               return md;
             });
}

void StatefulSessionFilterRegister(CoreConfiguration::Builder* builder) {
  StatefulSessionServiceConfigParser::Register(builder);
}

}  // namespace grpc_core
