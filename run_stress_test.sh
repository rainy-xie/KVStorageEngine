#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")" && pwd)
bin=${STRESS_BIN:-"$root/out/bin/stress_test"}
if [[ ! -x "$bin" ]]; then
    echo "Build the stress executable first: make (or set STRESS_BIN)." >&2
    exit 1
fi
if [[ "${1:-}" == "--help" ]]; then exec "$bin" "$@"; fi

log_root=${LOG_ROOT:-"$root/logs"}
mkdir -p "$log_root"
stamp=$(date +%Y%m%d_%H%M%S)
run_logs=$(mktemp -d "$log_root/run_${stamp}_XXXXXX")
echo "Logs: $run_logs"
disk_path="$PWD"
args=("$@")
for ((i=0; i<${#args[@]}; i++)); do
    if [[ ${args[i]} == "--dir" && $((i+1)) -lt ${#args[@]} ]]; then
        disk_path=${args[i+1]}
        while [[ ! -d "$disk_path" ]]; do disk_path=$(dirname "$disk_path"); done
    fi
done

pids=()
stress_pid=
cleanup() {
    if [[ -n "$stress_pid" ]]; then kill "$stress_pid" 2>/dev/null || true; fi
    for pid in "${pids[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

monitor() {
    local name=$1
    shift
    if command -v "$1" >/dev/null 2>&1; then
        LC_ALL=C S_TIME_FORMAT=ISO "$@" >"$run_logs/${name}_$stamp.log" 2>&1 &
        pids+=("$!")
    else
        echo "Skipping unavailable monitor: $1" >&2
    fi
}
if [[ $(uname -s) == Linux ]]; then
    monitor vmstat vmstat 60
    monitor iostat iostat -x 60
fi
# Child sleep is explicitly reaped when the disk sampler receives TERM.
(
    trap '[[ -z "${sleeper:-}" ]] || kill "$sleeper" 2>/dev/null; exit 0' TERM INT
    while true; do
        df -h "$disk_path"
        sleep 300 & sleeper=$!
        wait "$sleeper"
    done
) >"$run_logs/disk_usage_$stamp.log" 2>&1 &
pids+=("$!")

"$bin" "$@" >"$run_logs/stress_test_$stamp.log" 2>&1 &
stress_pid=$!
if [[ $(uname -s) == Linux ]]; then monitor pidstat pidstat -r -u 60 -p "$stress_pid"; fi
status=0
wait "$stress_pid" || status=$?
stress_pid=
cat "$run_logs/stress_test_$stamp.log"
exit "$status"
