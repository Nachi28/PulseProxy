#!/usr/bin/env bash
# examples/echo_proxy.sh
#
# Quickstart demo: run PulseProxy in front of a simple echo server.
#
# Terminal 1 (echo backend):
#   ncat -lk 8081 --sh-exec "cat"
#
# Terminal 2 (PulseProxy):
#   ./build/pulseproxy --listen-port 8080 --upstream-port 8081 --workers 1
#
# Terminal 3 (client):
#   echo "Hello PulseProxy!" | ncat 127.0.0.1 8080
#
# Expected output in Terminal 3:
#   Hello PulseProxy!

set -e

UPSTREAM_PORT=18081
LISTEN_PORT=18080
BINARY="$(dirname "$0")/../build/pulseproxy"

if [ ! -f "$BINARY" ]; then
  echo "Build PulseProxy first:"
  echo "  cmake -B build && cmake --build build --parallel"
  exit 1
fi

# Start a minimal echo backend.
ncat -lk "$UPSTREAM_PORT" --sh-exec "cat" &
ECHO_PID=$!
sleep 0.2

# Start PulseProxy (single worker for the demo).
"$BINARY" --listen-port "$LISTEN_PORT" \
           --upstream-port "$UPSTREAM_PORT" \
           --workers 1 \
           --stats-interval 0 &
PP_PID=$!
sleep 0.3

echo "--- Sending 'Hello PulseProxy!' through the proxy ---"
RESPONSE=$(echo "Hello PulseProxy!" | ncat 127.0.0.1 "$LISTEN_PORT" -w 2)
echo "Response: $RESPONSE"

kill "$PP_PID" "$ECHO_PID" 2>/dev/null
wait 2>/dev/null

if echo "$RESPONSE" | grep -q "Hello"; then
  echo "PASS"
else
  echo "FAIL: unexpected response"
  exit 1
fi
