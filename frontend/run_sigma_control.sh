#!/usr/bin/env bash
# Use system Qt rather than Qt libraries/plugins bundled with DAQ utilities.
set -euo pipefail
sigma_frontend_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -x "$sigma_frontend_dir/SigmaControl" ]]; then
    echo "Build SigmaControl first: cd $sigma_frontend_dir && qmake SigmaControl.pro && make -j4" >&2
    exit 1
fi
exec env -u LD_LIBRARY_PATH -u QT_PLUGIN_PATH -u QT_QPA_PLATFORM_PLUGIN_PATH \
    "$sigma_frontend_dir/SigmaControl" "$@"
