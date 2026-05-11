#!/usr/bin/env bash
# build.sh — convenience wrapper around the Makefile
set -euo pipefail

usage() {
    cat <<EOF
Usage: $(basename "$0") <command>

Commands:
  release   Build optimised release binary  -> build/release/simple_backup
  debug     Build debug binary (ASan/UBSan) -> build/debug/simple_backup
  package   Build Debian package            -> build/pkg/simple-backup_*.deb
  clean     Remove all build artefacts

EOF
    exit 1
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
    clean)
        make clean
        echo "Build directory removed."
        ;;
    *)
        echo "Unknown command: $CMD"
        usage
        ;;
esac
