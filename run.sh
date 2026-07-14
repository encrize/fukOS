#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
exec make run-limine CROSS=i686-elf- "$@"
