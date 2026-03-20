#!/bin/bash
# DPDK echo 端到端测试（无需 sudo，使用 unshare 网络命名空间）
# 用法: ./run_test.sh [udp|kcp]

set -e

MODE=${1:-udp}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../../../cmake-build-debug/apps"
DPDK_BIN="${BUILD_DIR}/example.dpdk"
TEST_PY="${SCRIPT_DIR}/test.py"
TAP_IP="10.0.0.1"
DPDK_IP="10.0.0.2"

if [ ! -f "$DPDK_BIN" ]; then
    echo "Build first: cmake --build cmake-build-debug --target example.dpdk"
    exit 1
fi

# Run the actual test inside a user+network namespace
exec unshare -r -n bash -c '
trap "kill %1 2>/dev/null; wait 2>/dev/null" EXIT

DPDK_BIN="'"$DPDK_BIN"'"
TEST_PY="'"$TEST_PY"'"
MODE="'"$MODE"'"
TAP_IP="'"$TAP_IP"'"
DPDK_IP="'"$DPDK_IP"'"

echo "=== DPDK ${MODE} echo test ==="

# Start server (DPDK_IP differs from TAP_IP to avoid kernel local delivery)
echo "[1/4] Starting DPDK ${MODE} server at ${DPDK_IP}..."
$DPDK_BIN $MODE server --local-ip $DPDK_IP 2>&1 &
sleep 2

if ! kill -0 %1 2>/dev/null; then
    echo "Server failed to start"
    exit 1
fi

# Configure TAP interface with kernel-side IP
echo "[2/4] Configuring dtap0 (${TAP_IP}/24)..."
ip addr add ${TAP_IP}/24 dev dtap0 2>/dev/null || true
ip link set dtap0 up

# Static ARP for DPDK IP (DPDK does not respond to ARP)
echo "[3/4] Adding static ARP for ${DPDK_IP}..."
TAP_MAC=$(ip link show dtap0 | grep link/ether | awk "{print \$2}")
ip neigh add $DPDK_IP lladdr $TAP_MAC dev dtap0

# Run client test
echo "[4/4] Running ${MODE} client..."
echo ""
python3 $TEST_PY $MODE $DPDK_IP 5
'
