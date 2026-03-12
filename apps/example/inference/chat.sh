#!/bin/bash
# Usage: ./chat.sh [model_name] [port]
#   model_name: 0.6b (default), 4b, 8b
#   port: default 8080

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SERVER="$PROJECT_DIR/cmake-build-debug/apps/example.inference"
MODEL_DIR="$PROJECT_DIR/third_party/models"

MODEL_NAME="${1:-4b}"
PORT="${2:-8080}"

case "$MODEL_NAME" in
    0.6b|0.6B) MODEL="$MODEL_DIR/Qwen3-0.6B-Q8_0.gguf" ;;
    4b|4B)     MODEL="$MODEL_DIR/Qwen3.5-4B-Q8_0.gguf" ;;
    8b|8B)     MODEL="$MODEL_DIR/Qwen3-8B-Q4_K_M.gguf" ;;
    *)         MODEL="$MODEL_NAME" ;;  # treat as path
esac

if [ ! -f "$SERVER" ]; then
    echo "Server binary not found: $SERVER"
    echo "Run: cmake --build cmake-build-debug --target example.inference"
    exit 1
fi

if [ ! -f "$MODEL" ]; then
    echo "Model not found: $MODEL"
    echo "Available: 0.6b, 4b, 8b"
    exit 1
fi

# Kill any existing server on this port
pkill -9 -f "example.inference.*$PORT" 2>/dev/null
sleep 0.5

echo "Starting server with $(basename "$MODEL") on port $PORT ..."
"$SERVER" "$MODEL" "$PORT" 999 2>&1 | sed 's/^/[server] /' &
SERVER_PID=$!

# Wait for server to be ready
for i in $(seq 1 30); do
    if ss -tlnp 2>/dev/null | grep -q ":$PORT "; then
        break
    fi
    sleep 1
done

if ! ss -tlnp 2>/dev/null | grep -q ":$PORT "; then
    echo "Server failed to start"
    kill -9 $SERVER_PID 2>/dev/null
    exit 1
fi

echo ""
echo "=== PUMP Inference Chat ==="
echo "Model: $(basename "$MODEL")"
echo "Type your message and press Enter. Ctrl+C to quit."
echo ""

# Run client
python3 "$SCRIPT_DIR/test_client.py" "$PORT"

# Cleanup
echo "Shutting down server..."
kill -9 $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null
echo "Done."
