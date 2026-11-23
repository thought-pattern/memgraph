#!/bin/bash
set -Eeuo pipefail
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
IFS=' '
# NOTE: docker_image_name could be local image build based on release/package images.
# NOTE: each line has to be under quotes, docker_container_type, script_name and docker_image_name separate with a space.
# "docker_container_type script_name docker_image_name"
#     docker_container_type OPTIONS:
#         * mgrun -> running plain/empty operating system for the purposes of testing native memgraph package
#         * mgbuild -> running the builder container to build memgraph inside it -> it's possible create builder images using release/package/run.sh
OPERATING_SYSTEMS=(
  # "mgrun amzn-2 amazonlinux:2"
  # "mgrun centos-7 centos:7"
  # "mgrun centos-9 dokken/centos-stream-9"
  # "mgrun centos-10 dokken/centos-stream-10"
  # "mgrun debian-10 debian:10"
  # "mgrun debian-11 debian:11"
  # "mgrun fedora-36 fedora:36"
  # "mgrun fedora-39 fedora:39"
  # "mgrun fedora-41 fedora:41"
  # "mgrun ubuntu-18.04 ubuntu:18.04"
  # "mgrun ubuntu-20.04 ubuntu:20.04"
  # "mgrun ubuntu-22.04 ubuntu:22.04"
  # "mgrun ubuntu-24.04 ubuntu:24.04"
  # "mgbuild debian-12 memgraph/memgraph-builder:v5_debian-12"
)

if [ ! "$(docker info)" ]; then
  echo "ERROR: Docker is required"
  exit 1
fi
print_help () {
  echo -e "$0 all [DEPS_GROUP]\t\t\t  => start + init all containers in the background"
  echo -e "$0 check [DEPS_GROUP]\t\t\t  => check all containers"
  echo -e "$0 delete\t\t\t\t  => stop + remove all containers"
  echo -e "$0 copy src_container dst_container => copy build package from src to dst container"
  exit 1
}

ALLOWED_DEPS_GROUPS=(
  TOOLCHAIN_RUN_DEPS
  TOOLCHAIN_BUILD_DEPS
  MEMGRAPH_BUILD_DEPS
  MEMGRAPH_TEST_DEPS
  MEMGRAPH_RUN_DEPS
)

resolve_deps_group () {
  local cnt_type="$1"
  local requested_group="${2:-}"
  if [[ -n "$requested_group" ]]; then
    local g
    for g in "${ALLOWED_DEPS_GROUPS[@]}"; do
      if [[ "$g" == "$requested_group" ]]; then
        echo "$requested_group"
        return 0
      fi
    done
    echo "ERROR: Unknown dependency group '$requested_group' (valid: ${ALLOWED_DEPS_GROUPS[*]})" >&2
    exit 1
  fi
  if [[ "$cnt_type" == "mgbuild" ]]; then
    echo "MEMGRAPH_BUILD_DEPS"
  else
    echo "MEMGRAPH_RUN_DEPS"
  fi
}

# NOTE: This is an idempotent operation!
# TODO(gitbuda): Consider making docker_run always delete + start a new container or add a new function.
docker_run () {
  cnt_type="$1"
  if [[ "$cnt_type" != "mgbuild" && "$cnt_type" != "mgrun" ]]; then
    echo "ERROR: Wrong docker_container_type -> valid options are mgbuild, mgrun"
    exit 1
  fi
  cnt_name="$2"
  cnt_image="$3"
  if [ ! "$(docker ps -q -f name=$cnt_name)" ]; then
      if [ "$(docker ps -aq -f status=exited -f name=$cnt_name)" ]; then
          echo "Cleanup of the old exited container..."
          docker rm $cnt_name
      fi
      if [[ "$cnt_type" == "mgbuild" ]]; then
        docker run -d --volume "$SCRIPT_DIR/../../:/memgraph" --network host --name "$cnt_name" "$cnt_image"
      fi
      if [[ "$cnt_type" == "mgrun" ]]; then
        docker run -d --volume "$SCRIPT_DIR/../../:/memgraph" --network host --name "$cnt_name" "$cnt_image" sleep infinity
      fi
  fi
  echo "The $cnt_image container is active under $cnt_name name!"
}

docker_exec () {
  cnt_name="$1"
  cnt_cmd="$2"
  if [ -t 0 ]; then
    docker exec -it "$cnt_name" bash -c "$cnt_cmd"
  else
    docker exec -i "$cnt_name" bash -c "$cnt_cmd"
  fi
}

docker_stop_and_rm () {
  cnt_name="$1"
  if [ "$(docker ps -q -f name=$cnt_name)" ]; then
      docker stop "$1"
  fi
  if [ "$(docker ps -aq -f status=exited -f name=$cnt_name)" ]; then
    docker rm "$1"
  fi
}

start_all () {
  local requested_group="${1:-}"
  for script_docker_pair in "${OPERATING_SYSTEMS[@]}"; do
    read -a script_docker <<< "$script_docker_pair"
    docker_container_type="${script_docker[0]}"
    script_name="${script_docker[1]}"
    docker_image="${script_docker[2]}"
    docker_name="${docker_container_type}_$script_name"
    deps_group="$(resolve_deps_group "$docker_container_type" "$requested_group")"
    echo ""
    echo "~~~~ OPERATING ON $docker_image as $docker_name..."
    docker_run "$docker_container_type" "$docker_name" "$docker_image"
    docker_exec "$docker_name" "/memgraph/environment/os/$script_name.sh install $deps_group"
    echo "---- DONE EVERYHING FOR $docker_image as $docker_name..."
    echo ""
  done
}

check_all () {
  local requested_group="${1:-}"
  for script_docker_pair in "${OPERATING_SYSTEMS[@]}"; do
    read -a script_docker <<< "$script_docker_pair"
    docker_container_type="${script_docker[0]}"
    script_name="${script_docker[1]}"
    docker_image="${script_docker[2]}"
    docker_name="${docker_container_type}_$script_name"
    deps_group="$(resolve_deps_group "$docker_container_type" "$requested_group")"
    echo ""
    echo "~~~~ OPERATING ON $docker_image as $docker_name..."
    docker_exec "$docker_name" "/memgraph/environment/os/$script_name.sh check $deps_group"
    echo "---- DONE EVERYHING FOR $docker_image as $docker_name..."
    echo ""
  done
}

delete_all () {
  for script_docker_pair in "${OPERATING_SYSTEMS[@]}"; do
    read -a script_docker <<< "$script_docker_pair"
    docker_container_type="${script_docker[0]}"
    script_name="${script_docker[1]}"
    docker_image="${script_docker[2]}"
    docker_name="${docker_container_type}_$script_name"
    docker_stop_and_rm "$docker_name"
    echo "~~~~ $docker_image as $docker_name DELETED"
  done
}

copy_build_package () {
  local src_container="$1"
  local dst_container="$2"
  local src="$src_container:/memgraph/build/output"
  local dst="$dst_container:/"
  local staging_dir
  staging_dir="$(mktemp -d "${TMPDIR:-/tmp}/memgraph-copy-output.XXXXXX")"
  (
    trap 'rm -rf "$staging_dir"' EXIT
    docker cp "$src" "$staging_dir/output"
    docker cp "$staging_dir/output" "$dst"
  )
}

if [ "$#" -eq 0 ]; then
  print_help
else
  case $1 in
    all)
      start_all "${2:-}"
    ;;
    check)
      check_all "${2:-}"
    ;;
    delete)
      delete_all
    ;;
    copy) # src_container dst_container
      if [ "$#" -ne 3 ]; then
        print_help
      fi
      copy_build_package "$2" "$3"
    ;;
    *)
      print_help
    ;;
  esac
fi
