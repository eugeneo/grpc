# Copyright 2016 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("//bazel:grpc_build_system.bzl", "grpc_cc_test", "grpc_package")

licenses(["notice"])

grpc_package(name = "test/core/util")

grpc_cc_test(
    name = "alloc_test",
    srcs = ["alloc_test.cc"],
    external_deps = ["gtest"],
    language = "C++",
    uses_event_engine = False,
    uses_polling = False,
    deps = [
        "//:gpr",
        "//test/core/test_util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "cpu_test",
    srcs = ["cpu_test.cc"],
    external_deps = ["gtest"],
    language = "C++",
    uses_event_engine = False,
    uses_polling = False,
    deps = [
        "//:gpr",
        "//test/core/test_util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "env_test",
    srcs = ["env_test.cc"],
    external_deps = [
        "absl/log:log",
        "gtest",
    ],
    language = "C++",
    uses_event_engine = False,
    uses_polling = False,
    deps = [
        "//:gpr",
        "//test/core/test_util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "spinlock_test",
    srcs = ["spinlock_test.cc"],
    external_deps = ["gtest"],
    language = "C++",
    uses_event_engine = False,
    uses_polling = False,
    deps = [
        "//:gpr",
        "//test/core/test_util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "string_test",
    srcs = ["string_test.cc"],
    external_deps = ["gtest"],
    language = "C++",
    uses_event_engine = False,
    uses_polling = False,
    deps = [
        "//:gpr",
        "//test/core/test_util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "sync_test",
    srcs = ["sync_test.cc"],
    external_deps = ["gtest"],
    language = "C++",
    uses_event_engine = False,
    uses_polling = False,
    deps = [
        "//:gpr",
        "//test/core/test_util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "time_test",
    srcs = ["time_test.cc"],
    external_deps = ["gtest"],
    language = "C++",
    uses_event_engine = False,
    uses_polling = False,
    deps = [
        "//:gpr",
        "//test/core/test_util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "unique_ptr_with_bitset_test",
    srcs = ["unique_ptr_with_bitset_test.cc"],
    external_deps = ["gtest"],
    language = "C++",
    uses_event_engine = False,
    uses_polling = False,
    deps = [
        "//:gpr_platform",
        "//src/core:unique_ptr_with_bitset",
    ],
)

grpc_cc_test(
    name = "useful_test",
    srcs = ["useful_test.cc"],
    external_deps = ["gtest"],
    language = "C++",
    uses_event_engine = False,
    uses_polling = False,
    deps = [
        "//:gpr_platform",
        "//src/core:useful",
    ],
)

grpc_cc_test(
    name = "ring_buffer_test",
    srcs = ["ring_buffer_test.cc"],
    external_deps = ["gtest"],
    language = "C++",
    uses_event_engine = False,
    uses_polling = False,
    deps = [
        "//:gpr_platform",
        "//src/core:ring_buffer",
    ],
)

grpc_cc_test(
    name = "lru_cache_test",
    srcs = ["lru_cache_test.cc"],
    external_deps = ["gtest"],
    language = "C++",
    uses_event_engine = False,
    uses_polling = False,
    deps = [
        "//src/core:lru_cache",
    ],
)
