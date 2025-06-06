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


function show_env() {
  set +e
  pwd
  df -h
  grep ^ /sys/block/*/queue/rotational

  cat /proc/cpuinfo | grep name | cut -f2 -d: | uniq -c
  lscpu
  cat /proc/meminfo
  uname -a
  hostname
  lsmod
  dmidecode | grep 'Product Name'
  free -mh
  cat /proc/loadavg
  ldd --version

  set -e
}

function wait_env() {
  local timeout='200'
  local failed='true'

  echo "=> wait for env available"

  for (( i = 0; i < "${timeout}"; i++ )); do
    if [[ -n $(cat ./log/tidb0/tidb.log | grep "server is running MySQL protocol") && \
          -n $(cat ./log/tiflash/tiflash.log | grep "Start to wait for terminal signal") ]]; then
        local failed='false'
        break
    fi

    if [ $((${i} % 10)) = 0 ] && [ ${i} -ge 10 ]; then
      echo "   #${i} waiting for env available"
    fi

    sleep 1
  done

  if [ "${failed}" == 'true' ]; then
    echo "   can not set up env" >&2
    exit 1
  else
    echo "   available"
  fi
}

function wait_tiflash_env() {
  local timeout='200'
  local failed='true'

  echo "=> wait for env available"

  for (( i = 0; i < "${timeout}"; i++ )); do
    if [[ -n $(cat ./log/tiflash/tiflash.log | grep "Start to wait for terminal signal") ]]; then
        local failed='false'
        break
    fi

    if [ $((${i} % 10)) = 0 ] && [ ${i} -ge 10 ]; then
      echo "   #${i} waiting for env available"
    fi

    sleep 1
  done

  if [ "${failed}" == 'true' ]; then
    echo "   can not set up env" >&2
    exit 1
  else
    echo "   available"
  fi
}

function wait_next_gen_env() {
  local timeout='200'
  local failed='true'

  echo "=> wait for env available"

  for (( i = 0; i < "${timeout}"; i++ )); do
    if [[ -n $(cat ./log/tidb0/tidb.log | grep "server is running MySQL protocol") && \
          -n $(cat ./log/tiflash-wn0/tiflash.log | grep "Start to wait for terminal signal") && \
          -n $(cat ./log/tiflash-cn0/tiflash.log | grep "Start to wait for terminal signal") ]]; then
        local failed='false'
        break
    fi

    if [ $((${i} % 10)) = 0 ] && [ ${i} -ge 10 ]; then
      echo "   #${i} waiting for env available"
    fi

    sleep 1
  done

  if [ "${failed}" == 'true' ]; then
    echo "   can not set up env" >&2
    exit 1
  else
    echo "   available"
  fi
}

function set_branch() {
  # XYZ_BRANCH: pd/tikv/tidb hash, default to `master`
  # BRANCH:     hash short cut, default to `master`
  if [ -n "$BRANCH" ]; then
    [ -z "$PD_BRANCH" ] && export PD_BRANCH="$BRANCH"
    [ -z "$TIKV_BRANCH" ] && export TIKV_BRANCH="$BRANCH"
    [ -z "$TIDB_BRANCH" ] && export TIDB_BRANCH="$BRANCH"
  fi
  echo "use branch \`${BRANCH-master}\` for ci test"
}

function clean_data_log() {
  rm -rf ./data ./log
}

function check_env() {
  local cur_dir=$(pwd)
  local prebuilt_bin_dir=$(realpath "${cur_dir}/../../tests/.build/tiflash")
  if [[ ! -d ${prebuilt_bin_dir} ]]; then
    echo "No pre-build tiflash binary directory: ${prebuilt_bin_dir}"
    exit -1
  else
    echo "Running tests with pre-built tiflash binary: ${prebuilt_bin_dir}/tiflash"
    ${prebuilt_bin_dir}/tiflash --version
  fi
}

function check_docker_compose() {
  # Try to use these compose tools:
  # - `docker-compose`, the original compose tool on CI
  # - `podman compose`, the podman compose tool, which is compatible with docker compose,
  #   and supports rootless mode
  # - `docker compose`, the new docker provide compose command, which is compatible
  #   with `docker-compose`
  if command -v docker-compose &>/dev/null; then
    echo "docker-compose is installed."
    export COMPOSE="docker-compose"
  else
    if command -v podman &>/dev/null; then
      echo "podman is installed, using it as docker-compose."
      export COMPOSE="podman compose"
    else
      if command -v docker &>/dev/null; then
        echo "docker compose is installed."
        export COMPOSE="docker compose"
      else
        echo "Neither docker-compose nor docker noo podman could be found, please install one of them first."
        exit 1
      fi
    fi
  fi
}

export -f show_env
export -f wait_env
export -f wait_tiflash_env
export -f wait_next_gen_env
export -f set_branch
export -f clean_data_log
export -f check_env
export -f check_docker_compose
