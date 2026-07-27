#!/usr/bin/env bash
# Builds (if needed) and runs the pure C++ demo: mini_ics_server --demo
# (which attaches the software endpoint model in place of the SV
# testbench) plus mini_ics_host_client, connected over a real Unix
# socket. This exercises the full protocol path without requiring any
# HDL simulator, and is the fastest way to sanity-check a protocol or
# server change.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${MINI_ICS_BUILD_DIR:-${ROOT_DIR}/build}"
SOCKET_PATH="${MINI_ICS_SOCKET_PATH:-/tmp/mini_ics_cpp_demo_$$.sock}"
LOG_LEVEL="${MINI_ICS_LOG_LEVEL:-info}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "${BUILD_DIR}" -j"$(nproc)" --target mini_ics_server mini_ics_host_client

rm -f "${SOCKET_PATH}"

SERVER_BIN="${BUILD_DIR}/mini_ics_server"
CLIENT_BIN="${BUILD_DIR}/mini_ics_host_client"

"${SERVER_BIN}" --demo --socket "${SOCKET_PATH}" --log-level "${LOG_LEVEL}" &
SERVER_PID=$!

cleanup() {
  if kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  rm -f "${SOCKET_PATH}"
}
trap cleanup EXIT

# Give the server a moment to create and bind the socket before the
# client tries to connect.
for _ in $(seq 1 50); do
  [ -S "${SOCKET_PATH}" ] && break
  sleep 0.1
done

set +e
"${CLIENT_BIN}" --socket "${SOCKET_PATH}" --timeout-ms 5000 --log-level "${LOG_LEVEL}"
CLIENT_RC=$?
set -e

wait "${SERVER_PID}"
SERVER_RC=$?

if [ "${CLIENT_RC}" -eq 0 ] && [ "${SERVER_RC}" -eq 0 ]; then
  echo "run_cpp_demo.sh: PASS"
  exit 0
else
  echo "run_cpp_demo.sh: FAIL (client_rc=${CLIENT_RC} server_rc=${SERVER_RC})"
  exit 1
fi
