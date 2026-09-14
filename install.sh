#!/bin/sh
# shimback installer -- downloads the right prebuilt binary for this
# machine from GitHub Releases and hands it to `shimback install`, which
# does the rest (copies itself to a stable location, sets up PATH, installs
# the man page). Usage:
#
#   curl -fsSL https://raw.githubusercontent.com/pnavais/shimback/master/install.sh | sh
#
# Any extra arguments are forwarded to `shimback install` as-is, e.g.:
#
#   curl -fsSL .../install.sh | sh -s -- --prefix ~/.local
#
# Pin a specific release instead of the latest one via SHIMBACK_VERSION:
#
#   curl -fsSL .../install.sh | SHIMBACK_VERSION=v0.1.0 sh
set -eu

REPO="pnavais/shimback"

is_tty() {
    [ -t 1 ]
}

use_color() {
    [ -z "${NO_COLOR:-}" ] && is_tty
}

print_banner() {
    if use_color; then
        printf '\033[1;34m'
    fi
    cat <<'BANNER'
   _____ __  ________  _______  ___   ________ __
  / ___// / / /  _/  |/  / __ )/   | / ____/ //_/
  \__ \/ /_/ // // /|_/ / __  / /| |/ /   / ,<
 ___/ / __  // // /  / / /_/ / ___ / /___/ /| |
/____/_/ /_/___/_/  /_/_____/_/  |_\____/_/ |_|
BANNER
    if use_color; then
        printf '\033[0m'
    fi
    echo "  >> run a primary command, transparently fall back to another >>"
    echo "  Copyright (c) 2026 pnavais -- MIT OR Apache-2.0"
    echo
}

die() {
    echo "shimback-install: $*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "'$1' is required but not found on \$PATH"
}

detect_os() {
    case "$(uname -s)" in
        Darwin) echo "macos" ;;
        Linux) echo "linux" ;;
        *) die "unsupported OS '$(uname -s)' -- shimback ships prebuilt binaries for macOS and Linux only. Build from source instead: https://github.com/$REPO#building" ;;
    esac
}

detect_arch() {
    case "$(uname -m)" in
        x86_64 | amd64) echo "x86_64" ;;
        arm64 | aarch64) echo "arm64" ;;
        *) die "unsupported architecture '$(uname -m)' -- shimback ships x86_64 and arm64 binaries only. Build from source instead: https://github.com/$REPO#building" ;;
    esac
}

main() {
    print_banner

    need_cmd curl
    need_cmd tar

    os="$(detect_os)"
    arch="$(detect_arch)"
    asset="shimback-${os}-${arch}.tar.gz"

    version="${SHIMBACK_VERSION:-latest}"
    if [ "$version" = "latest" ]; then
        url="https://github.com/$REPO/releases/latest/download/$asset"
    else
        url="https://github.com/$REPO/releases/download/$version/$asset"
    fi

    tmpdir="$(mktemp -d)"
    trap 'rm -rf "$tmpdir"' EXIT INT TERM

    echo "Downloading $asset ($version)..."
    if ! curl -fsSL "$url" -o "$tmpdir/$asset"; then
        die "failed to download $url -- check that a '$version' release exists for $os/$arch"
    fi

    tar -xzf "$tmpdir/$asset" -C "$tmpdir"
    bin="$tmpdir/shimback-${os}-${arch}/shimback"
    [ -x "$bin" ] || die "downloaded archive didn't contain an executable 'shimback' binary"

    echo "Installing..."
    "$bin" install "$@"
}

main "$@"
