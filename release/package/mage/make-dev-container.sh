#!/bin/bash

################################################################################
# Use this script within the `prod` container to convert it to a dev container #
# Optionally skip toolchain download with --no-toolchain                       #
# run me as root!                                                              #
################################################################################

set -euo pipefail

TOOLCHAIN_SHA256="${TOOLCHAIN_SHA256:-}"
SKIP_TOOLCHAIN=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-toolchain)
      SKIP_TOOLCHAIN=true
      shift
      ;;
    --toolchain-sha256)
      if [[ $# -lt 2 || -z "${2:-}" || "${2:-}" == -* ]]; then
        echo "Error: --toolchain-sha256 requires a 64-character hex SHA-256 digest" >&2
        exit 1
      fi
      TOOLCHAIN_SHA256="$2"
      shift 2
      ;;
    --toolchain-sha256=*)
      TOOLCHAIN_SHA256="${1#*=}"
      shift
      ;;
    *)
      echo "Error: Unknown option '$1'" >&2
      exit 1
      ;;
  esac
done

# install dependencies
apt-get update
apt-get install -y libpython${PY_VERSION:-$(python3 --version | sed 's/Python //')} \
  libcurl4 libssl-dev openssl build-essential cmake curl g++ python3  \
  python3-pip python3-setuptools python3-dev clang git unixodbc-dev \
  libboost-all-dev uuid-dev gdb procps libc6-dbg libxmlsec1-dev xmlsec1 \
  --no-install-recommends


# Function to detect architecture and set the appropriate toolchain URL
get_toolchain_url() {
  version=$1
  arch=$(uname -m)

  # Determine the file name based on the architecture.
  case "$arch" in
    x86_64)
      file="toolchain-v${version}-binaries-ubuntu-24.04-amd64.tar.gz"
      ;;
    aarch64)
      file="toolchain-v${version}-binaries-ubuntu-24.04-arm64.tar.gz"
      ;;
    *)
      echo "Unsupported architecture: $arch" >&2
      exit 1
      ;;
  esac

  # Construct the mgdeps-cache file URL.
  mgdeps_url="http://mgdeps-cache:8000/file/${file}"

  # Check if the file is available on the cache using a HEAD request.
  if curl --head --silent --fail "$mgdeps_url" -o /dev/null; then
    echo "$mgdeps_url"
  else
    # Fallback to the S3 URL if the cache doesn't have the file.
    s3_url="https://s3-eu-west-1.amazonaws.com/deps.memgraph.io/toolchain-v${version}/${file}"
    echo "$s3_url"
  fi
}


toolchain_version=7
if [[ "$SKIP_TOOLCHAIN" == false ]]; then
  if [[ ! "$TOOLCHAIN_SHA256" =~ ^[0-9a-fA-F]{64}$ ]]; then
    echo "Error: A valid 64-character hex SHA-256 digest is required via --toolchain-sha256 or TOOLCHAIN_SHA256 to verify the toolchain archive." >&2
    exit 1
  fi
  TOOLCHAIN_URL=$(get_toolchain_url "$toolchain_version")
  echo "Downloading toolchain from: $TOOLCHAIN_URL"
  (
    set -euo pipefail
    tmp_toolchain_dir="$(mktemp -d "${TMPDIR:-/tmp}/toolchain.XXXXXX")"
    tmp_toolchain="$tmp_toolchain_dir/toolchain.tar.gz"
    trap 'rm -rf "$tmp_toolchain_dir"' EXIT
    if [[ "$TOOLCHAIN_URL" == https://* ]]; then
      curl -fL --proto '=https' --proto-redir '=https' --tlsv1.2 "$TOOLCHAIN_URL" -o "$tmp_toolchain"
    else
      curl -fL "$TOOLCHAIN_URL" -o "$tmp_toolchain"
    fi
    expected_sha256="$(printf '%s' "$TOOLCHAIN_SHA256" | tr '[:upper:]' '[:lower:]')"
    if command -v sha256sum >/dev/null 2>&1; then
      printf '%s  %s\n' "$expected_sha256" "$tmp_toolchain" | sha256sum -c -
    else
      printf '%s  %s\n' "$expected_sha256" "$tmp_toolchain" | shasum -a 256 -c -
    fi
    tar xzvfm "$tmp_toolchain" -C /opt
  )
else
  echo "Skipping toolchain download (--no-toolchain)"
fi


# ENV from the Dockerfile; empty when the build arg wasn't passed.
MEMGRAPH_REF="${MEMGRAPH_REF:-master}"

echo "Cloning Memgraph repo commit/tag/branch: $MEMGRAPH_REF"
cd /root
git clone https://github.com/memgraph/memgraph.git
cd /root/memgraph
git checkout "$MEMGRAPH_REF"

cd /

echo "Copying repo files to /mage"
# Copy files without overwriting existing ones
cp -r --update=none /root/memgraph/. /mage/

# Change ownership of everything in /mage to memgraph
chown -R memgraph: /mage/

# remove git repo from `/root`
rm -rf /root/memgraph

if [[ "$SKIP_TOOLCHAIN" == false ]]; then
  # install toolchain run dependencies
  ./mage/environment/os/install_deps.sh install TOOLCHAIN_RUN_DEPS
fi

# install Rust as `memgraph` user - this is required for building mage. The
# version and the hash-verified rustup-init come from environment/util.sh, so
# the dev container gets the same Rust as the mgbuild containers.
if ! grep -q "^MG_RUSTUP_SHA256_$(uname -m)=" /mage/environment/util.sh; then
  echo "/mage/environment/util.sh at $MEMGRAPH_REF has no pinned rustup-init; refusing to install Rust" >&2
  exit 1
fi
su - memgraph -s /bin/bash -c 'source /mage/environment/util.sh && retry_install install_rust "$MG_RUST_VERSION" && echo "export PATH=\$HOME/.cargo/bin:\$PATH" >> $HOME/.bashrc'

# build everything again (because it isn't copied into the prod image);
# MAGE modules end up in /mage/build/mage/dist
if [[ "$SKIP_TOOLCHAIN" == false && -f "/opt/toolchain-v${toolchain_version}/activate" ]]; then
  build_cmd="source /opt/toolchain-v${toolchain_version}/activate && cd /mage && ./build.sh --mage on --build-type ${BUILD_TYPE:-RelWithDebInfo}"
else
  build_cmd="cd /mage && ./build.sh --mage on --build-type ${BUILD_TYPE:-RelWithDebInfo} -DMG_BUNDLED_TOOLCHAIN=OFF"
fi
su - memgraph -c "bash -c '$build_cmd'"


echo "Done!"
