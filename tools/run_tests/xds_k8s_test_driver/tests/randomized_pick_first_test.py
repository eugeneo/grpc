# Copyright 2023 gRPC authors.
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
import logging
from typing import List

from absl import flags
from absl.testing import absltest

from framework import xds_k8s_flags
from framework import xds_k8s_testcase
from framework import xds_url_map_testcase
from framework.helpers import skips

logger = logging.getLogger(__name__)
flags.adopt_module_key_flags(xds_k8s_testcase)
flags.mark_flag_as_required("server_image_canonical")

# Type aliases
RpcTypeUnaryCall = xds_url_map_testcase.RpcTypeUnaryCall
RpcTypeEmptyCall = xds_url_map_testcase.RpcTypeEmptyCall
_XdsTestServer = xds_k8s_testcase.XdsTestServer
_XdsTestClient = xds_k8s_testcase.XdsTestClient
_Lang = skips.Lang

# Testing consts
_QPS = 100
_REPLICA_COUNT = 5


class RandomizedPickFirstTest(xds_k8s_testcase.RegularXdsKubernetesTestCase):
    """
    Implementation of https://github.com/grpc/grpc/blob/master/doc/xds-test-descriptions.md#outlier_detection

    This test verifies that the client applies the outlier detection
    configuration and temporarily drops traffic to a server that fails
    requests.
    """

    @staticmethod
    def is_supported(config: skips.TestConfig) -> bool:
        if config.client_lang in _Lang.CPP | _Lang.PYTHON:
            return config.version_gte("v1.56.x")
        return False

    def test_randomized_pick_first(self) -> None:
        with self.subTest("00_create_health_check"):
            self.td.create_health_check()

        with self.subTest("01_create_backend_service"):
            self.td.create_backend_service(
                locality_lb_policies=[
                    {
                        "customPolicy": {
                            "name": "pick_first",
                            "data": '{ "shuffleAddressList": true }',
                        },
                    },
                ]
            )

        with self.subTest("02_create_url_map"):
            self.td.create_url_map(self.server_xds_host, self.server_xds_port)

        with self.subTest("03_create_target_proxy"):
            self.td.create_target_proxy()

        with self.subTest("04_create_forwarding_rule"):
            self.td.create_forwarding_rule(self.server_xds_port)

        test_servers: List[_XdsTestServer]
        with self.subTest("05_start_test_servers"):
            test_servers = self.startTestServers(replica_count=_REPLICA_COUNT)

        with self.subTest("06_add_server_backends_to_backend_services"):
            self.setupServerBackends()

        test_client: _XdsTestClient
        with self.subTest("07_start_test_client"):
            test_client = self.startTestClient(test_servers[0], qps=_QPS)

        with self.subTest("08_test_client_xds_config_exists"):
            self.assertXdsConfigExists(test_client)

        with self.subTest("09_test_servers_received_rpcs_from_test_client"):
            test_client.update_config.configure(
                rpc_types=rpc_types,
                metadata=(
                    (
                        RpcTypeUnaryCall,
                        "rpc-behavior",
                        "error-code-2",
                    ),
                ),
            )
            self.assertRpcsEventuallyGoToGivenServers(test_client, test_servers)

        # rpc_types = (RpcTypeUnaryCall,)
        # with self.subTest("10_chosen_server_removed_by_outlier_detection"):
        #     test_client.update_config.configure(
        #         rpc_types=rpc_types,
        #         metadata=(
        #             (
        #                 RpcTypeUnaryCall,
        #                 "rpc-behavior",
        #                 "error-code-2",
        #             ),
        #         ),
        #     )
        #     self.assertRpcsEventuallyGoToGivenServers(
        #         test_client, test_servers[1:]
        #     )

        # with self.subTest("11_ejected_server_returned_after_failures_stopped"):
        #     test_client.update_config.configure(rpc_types=rpc_types)
        #     self.assertRpcsEventuallyGoToGivenServers(test_client, test_servers)


if __name__ == "__main__":
    absltest.main(failfast=True)
