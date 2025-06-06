#!/bin/bash
# Copyright 2023 PingCAP, Inc.
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

source ../docker/util.sh

set_branch

set -xe

check_env
check_docker_compose

${COMPOSE} -f mock-test.yaml down
clean_data_log

# (only tics0 up)
${COMPOSE} -f mock-test.yaml up -d
wait_tiflash_env
${COMPOSE} -f mock-test.yaml exec -T tics0 bash -c 'cd /tests ; ./run-test.sh delta-merge-test'
${COMPOSE} -f mock-test.yaml down
clean_data_log
