#!/usr/bin/env bash
# build.sh — convenience wrapper around the Makefile
set -euo pipefail

usage() {
    cat <<EOF
Usage: $(basename "$0") <command>

Commands:
  release   Build optimised release binary  -> build/release/simple_backup
  debug     Build debug binary (ASan/UBSan) -> build/debug/simple_backup
  package   Build Debian package            -> dist/simple-backup_*.deb
  check     Report which build dependencies are missing
  clean     Remove build/ and dist/

Environment:
  REMOTE_FILES=0    Build and check without the remote_files library, so
                    without WebDAV and SFTP destinations
  PKG_CONFIG_PATH   Where pkg-config looks for remote_files, such as
                    ~/remote_files/lib/pkgconfig for a deploy outside /usr

EOF
    exit 1
}

ok()   { echo "[OK]    $*"; }
warn() { echo "[WARN]  $*"; }
fail() { echo "[ERR]   $*"; }

# Compiles a one line program, which finds headers exactly where the compiler
# looks for them.
has_header() {
    echo "#include <$1>" | g++ -std=c++17 -fsyntax-only -x c++ - 2>/dev/null
}

check() {
    local missing=0

    if ! command -v g++ &>/dev/null; then
        fail "g++ not found (install: sudo apt install g++)"
        missing=1
    elif ! echo 'int main(){}' | g++ -std=c++17 -x c++ - -o /dev/null 2>/dev/null; then
        fail "g++ $(g++ -dumpfullversion) has no C++17 support"
        missing=1
    else
        ok "g++ $(g++ -dumpfullversion)"
    fi

    if command -v make &>/dev/null; then
        ok "make"
    else
        fail "make not found (install: sudo apt install make)"
        missing=1
    fi

    if has_header nlohmann/json.hpp; then
        ok "nlohmann/json headers"
    else
        fail "nlohmann/json headers not found (install: sudo apt install nlohmann-json3-dev)"
        missing=1
    fi

    if [[ "${REMOTE_FILES:-1}" == "0" ]]; then
        warn "REMOTE_FILES=0, so remote_files is skipped and WebDAV and SFTP destinations won't load"
    elif ! command -v pkg-config &>/dev/null; then
        fail "pkg-config not found, and the build finds remote_files through it (install: sudo apt install pkg-config)"
        missing=1
    elif ! pkg-config --exists remote_files; then
        fail "remote_files not found by pkg-config. Install libremote-files-dev, set PKG_CONFIG_PATH to a deploy"
        fail "such as ~/remote_files/lib/pkgconfig, or build with REMOTE_FILES=0"
        missing=1
    else
        local version_header
        version_header="$(pkg-config --variable=includedir remote_files)/remote_files/version.h"
        ok "remote_files $(pkg-config --modversion remote_files) ($(realpath -m "$(pkg-config --variable=libdir remote_files)"))"

        if grep -q 'define REMOTE_FILES_HAS_CURL 1' "$version_header" 2>/dev/null; then
            ok "remote_files has WebDAV support"
        else
            warn "remote_files has no WebDAV support, so a webdav destination won't load"
        fi

        if grep -q 'define REMOTE_FILES_HAS_SSH 1' "$version_header" 2>/dev/null; then
            ok "remote_files has SFTP support"
        else
            warn "remote_files has no SFTP support, so an sftp destination won't load. Rebuild remote_files with libssh"
        fi
    fi

    if command -v dpkg-deb &>/dev/null; then
        ok "dpkg-deb, for the package command"
    else
        warn "dpkg-deb not found, so the package command won't run (install: sudo apt install dpkg)"
    fi

    echo ""
    if [[ "$missing" -ne 0 ]]; then
        fail "Missing build dependencies, see above"
        exit 1
    fi

    ok "Everything the build needs is installed"
}

CMD=${1:-}
[[ -z "$CMD" ]] && usage

case "$CMD" in
    release)
        make release
        echo "Binary: build/release/simple_backup"
        ;;
    debug)
        make debug
        echo "Binary: build/debug/simple_backup"
        ;;
    package)
        # dpkg-deb is required
        if ! command -v dpkg-deb &>/dev/null; then
            echo "ERROR: dpkg-deb not found. Install: sudo apt-get install dpkg"
            exit 1
        fi
        make package
        ;;
    check)
        check
        ;;
    clean)
        make clean
        echo "Removed build/ and dist/."
        ;;
    *)
        echo "Unknown command: $CMD"
        usage
        ;;
esac
