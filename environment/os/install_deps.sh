#!/usr/bin/env bash
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

# Source the util.sh file to get the parse_operating_system function
source "$SCRIPT_DIR/../util.sh"

# Distro scripts no longer exposed through install_deps.sh (the files remain
# and can still be run directly).
DEPRECATED_OS=(
    ubuntu-20.04
)

# Every distro script in this directory is a supported OS; the filename is
# the OS name.
SUPPORTED_OS=()
for script in "$SCRIPT_DIR"/*.sh; do
    name="$(basename "$script" .sh)"
    case "$name" in
        lib|install_deps|template|test|run) continue ;;
    esac
    for deprecated in "${DEPRECATED_OS[@]}"; do
        [[ "$name" == "$deprecated" ]] && continue 2
    done
    SUPPORTED_OS+=("$name")
done

# Define toolchain download URLs and optional pinned SHA-256 digests for supported Linux architectures
toolchain_url_for_arch() {
    case "$1" in
        x86_64) echo "${MG_TOOLCHAIN_URL_x86_64:-https://s3.eu-west-1.amazonaws.com/deps.memgraph.io/toolchain-v8/toolchain-v8-binaries-x86_64.tar.gz}" ;;
        aarch64) echo "${MG_TOOLCHAIN_URL_aarch64:-https://s3.eu-west-1.amazonaws.com/deps.memgraph.io/toolchain-v8/toolchain-v8-binaries-aarch64.tar.gz}" ;;
        *) echo "" ;;
    esac
}

toolchain_sha256_for_arch() {
    case "$1" in
        x86_64) echo "${MG_TOOLCHAIN_SHA256_x86_64:-}" ;;
        aarch64) echo "${MG_TOOLCHAIN_SHA256_aarch64:-}" ;;
        *) echo "" ;;
    esac
}

# Parse command line arguments to extract --set-os and --toolchain-sha256 flags
SET_OS=""
TOOLCHAIN_SHA256_OVERRIDE="${MG_TOOLCHAIN_SHA256:-}"
ARGS=()

while [[ $# -gt 0 ]]; do
    case $1 in
        --set-os)
            if [[ $# -lt 2 || -z "${2:-}" || "${2:-}" == -* ]]; then
                echo "Error: --set-os requires an OS argument (e.g. --set-os ubuntu-24.04)" >&2
                exit 1
            fi
            SET_OS="$2"
            shift 2
            ;;
        --set-os=*)
            SET_OS="${1#*=}"
            if [[ -z "$SET_OS" ]]; then
                echo "Error: --set-os requires an OS argument (e.g. --set-os=ubuntu-24.04)" >&2
                exit 1
            fi
            shift
            ;;
        --toolchain-sha256)
            if [[ $# -lt 2 || -z "${2:-}" || "${2:-}" == -* ]]; then
                echo "Error: --toolchain-sha256 requires a 64-character hex SHA-256 digest" >&2
                exit 1
            fi
            TOOLCHAIN_SHA256_OVERRIDE="$2"
            shift 2
            ;;
        --toolchain-sha256=*)
            TOOLCHAIN_SHA256_OVERRIDE="${1#*=}"
            if [[ -z "$TOOLCHAIN_SHA256_OVERRIDE" ]]; then
                echo "Error: --toolchain-sha256 requires a 64-character hex SHA-256 digest" >&2
                exit 1
            fi
            shift
            ;;
        -h|--help)
            cat <<EOF
Usage:
    ./install_deps.sh [command] [dependency_set] [--set-os <os>] [--toolchain-sha256 <sha256>]

Options:
    --set-os <os>              Override OS detection and use specified OS.
                               Supported values: ${SUPPORTED_OS[*]}
    --toolchain-sha256 <hash>  Expected SHA-256 digest of the toolchain archive
                               required when running 'prepare TOOLCHAIN_RUN_DEPS'.

Commands:
    prepare       Downloads, verifies, and extracts specified dependencies.
                  Supported only for Linux TOOLCHAIN_RUN_DEPS.
    install       Install the specified dependencies.
    check         Check the specified dependencies.

Dependency sets:
    TOOLCHAIN_RUN_DEPS       Dependencies required for the toolchain runtime.
    MEMGRAPH_BUILD_DEPS      Dependencies required for building Memgraph.
    MEMGRAPH_TEST_DEPS       Extra dependencies (on top of MEMGRAPH_BUILD_DEPS)
                             required to run the test suites.
    MEMGRAPH_RUN_DEPS        Dependencies required to run a Memgraph package.

Examples:
    sudo ./install_deps.sh prepare TOOLCHAIN_RUN_DEPS --toolchain-sha256 <sha256>
    sudo ./install_deps.sh check TOOLCHAIN_RUN_DEPS
    sudo ./install_deps.sh install TOOLCHAIN_RUN_DEPS
    sudo ./install_deps.sh check MEMGRAPH_BUILD_DEPS
    sudo ./install_deps.sh install MEMGRAPH_BUILD_DEPS
    sudo ./install_deps.sh install TOOLCHAIN_RUN_DEPS --set-os ubuntu-24.04
    sudo ./install_deps.sh --set-os ubuntu-24.04 install TOOLCHAIN_RUN_DEPS


Link to create new github issue: https://github.com/memgraph/memgraph/issues/new?title=install-deps.sh%20...&assignee=gitbuda&body=%0A%0A%0A---%0AI%27m+a+human.+Please+be+nice.
EOF
            exit 0
            ;;
        *)
            ARGS+=("$1")
            shift
            ;;
    esac
done

# Set the arguments back to $@ for the rest of the script
set -- ${ARGS[@]+"${ARGS[@]}"}

# Function to check and install dependencies for a given distribution script
run_script() {
    local distro_script="$1"
    shift
    echo "Running script for $distro_script $*"
    "$SCRIPT_DIR/$distro_script" "$@"
}

verify_sha256_file() {
    local expected_hash
    expected_hash="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
    local file_path="$2"
    if command -v sha256sum >/dev/null 2>&1; then
        printf '%s  %s\n' "$expected_hash" "$file_path" | sha256sum -c -
    elif command -v shasum >/dev/null 2>&1; then
        printf '%s  %s\n' "$expected_hash" "$file_path" | shasum -a 256 -c -
    else
        echo "Error: Neither sha256sum nor shasum is installed to verify toolchain archive." >&2
        return 1
    fi
}

# Function for 'prepare' command to download, verify, and extract the Linux toolchain
prepare_toolchain() {
    local os_arch="$1"
    if [[ "$os_arch" == freebsd-* || ( -z "$SET_OS" && "$(uname -s)" != "Linux" ) ]]; then
        echo "Error: Bundled Linux toolchain preparation is not supported on '$os_arch' (host kernel: $(uname -s))." >&2
        exit 1
    fi

    local raw_arch
    raw_arch="$(uname -m)"
    local toolchain_arch=""
    case "$raw_arch" in
        x86_64) toolchain_arch="x86_64" ;;
        aarch64) toolchain_arch="aarch64" ;;
        *)
            echo "Error: Unsupported architecture '$raw_arch' for toolchain preparation on '$os_arch' (supported: x86_64, aarch64)." >&2
            exit 1
            ;;
    esac

    local toolchain_url
    toolchain_url="$(toolchain_url_for_arch "$toolchain_arch")"
    if [ -z "$toolchain_url" ]; then
        echo "No toolchain URL found for $os_arch ($toolchain_arch). Please ensure your OS and architecture are supported." >&2
        exit 1
    fi

    local expected_sha256="${TOOLCHAIN_SHA256_OVERRIDE:-$(toolchain_sha256_for_arch "$toolchain_arch")}"
    if [[ ! "$expected_sha256" =~ ^[0-9a-fA-F]{64}$ ]]; then
        echo "Error: A valid 64-character hex SHA-256 digest is required to verify '$toolchain_url' (pass --toolchain-sha256 <sha256> or set MG_TOOLCHAIN_SHA256)." >&2
        exit 1
    fi

    local dest_dir="${MG_TOOLCHAIN_DEST_DIR:-/opt}"
    (
        set -euo pipefail
        local tmp_dir
        tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/memgraph-toolchain.XXXXXX")"
        trap 'rm -rf "$tmp_dir"' EXIT
        local archive_path="$tmp_dir/toolchain.tar.gz"

        echo "Setting up toolchain for $os_arch ($toolchain_arch)..."
        curl -fL --proto '=https' --proto-redir '=https' --tlsv1.2 "$toolchain_url" --output "$archive_path" || {
            echo "Failed to download toolchain. Please check your internet connection or the URL and try again." >&2
            exit 1
        }

        echo "Verifying toolchain archive SHA-256 digest..."
        verify_sha256_file "$expected_sha256" "$archive_path" || {
            echo "Failed to verify toolchain SHA-256 digest; refusing to extract archive." >&2
            exit 1
        }

        echo "Extracting toolchain to $dest_dir..."
        tar xzvf "$archive_path" -C "$dest_dir" || {
            echo "Failed to extract toolchain. Please check the archive and your permissions." >&2
            exit 1
        }
    ) || exit 1

    echo "Toolchain setup complete."
}

# Detect OS, version, and architecture
if [[ -n "$SET_OS" ]]; then
    # Parse OS and VER from SET_OS string (e.g., "ubuntu-24.04" -> OS="ubuntu", VER="24.04")
    # Handle formats like: ubuntu-24.04, ubuntu-24.04-arm, centos-9, debian-11-arm
    if [[ "$SET_OS" =~ ^([a-z]+)-([0-9.]+)$ ]]; then
        OS="${BASH_REMATCH[1]}"
        VER="${BASH_REMATCH[2]}"
    else
        echo "Error: Invalid OS format: $SET_OS. Expected format: os-version"
        exit 1
    fi
elif [[ "$OSTYPE" == "freebsd"* ]]; then
    # No /etc/os-release here; the shared detection reads uname instead.
    parse_operating_system
elif [ -f /etc/os-release ]; then
    . /etc/os-release
    parse_operating_system
elif type lsb_release >/dev/null 2>&1; then
    OS=$(lsb_release -si)
    VER=$(lsb_release -sr)
elif [ -f /etc/lsb-release ]; then
    . /etc/lsb-release
    OS=$DISTRIB_ID
    VER=$DISTRIB_RELEASE
elif [ -f /etc/debian_version ]; then
    OS=Debian
    VER=$(cat /etc/debian_version)
else
    echo "OS not identified"
    exit 1
fi

# Normalize OS name to lowercase. One distro script serves every
# architecture, so the OS identifier carries no arch suffix.
OS=$(echo "$OS" | tr '[:upper:]' '[:lower:]')
OS_ARCH="${OS}-${VER}"

OS_ARCH_SCRIPT="${OS_ARCH}.sh"

# Check if OS_ARCH is in the SUPPORTED_OS array
is_supported=false
for supported_os in "${SUPPORTED_OS[@]}"; do
    if [[ "$OS_ARCH" == "$supported_os" ]]; then
        is_supported=true
        break
    fi
done

if [[ "$1" == "prepare" ]]; then
    if [[ "$2" == "TOOLCHAIN_RUN_DEPS" ]]; then
        if [[ "$is_supported" == true ]]; then
            prepare_toolchain "$OS_ARCH"
        else
            echo "Unsupported OS: $OS_ARCH. The 'prepare' command cannot proceed."
            echo "Supported OS values: ${SUPPORTED_OS[*]}"
            exit 1
        fi
    else
        echo "Error: The 'prepare' command only supports 'TOOLCHAIN_RUN_DEPS' as the second argument."
        exit 1
    fi
else
    # If supported, run the script with all original arguments
    if [[ "$is_supported" == true ]]; then
        if [ -z "$SET_OS" ]; then
            run_script "$OS_ARCH_SCRIPT" "$@"
        else
            run_script "$OS_ARCH_SCRIPT" "$@" --skip-check
        fi
    else
        echo "Unsupported OS: $OS_ARCH"
        echo "Supported OS values: ${SUPPORTED_OS[*]}"
        exit 1
    fi
fi
