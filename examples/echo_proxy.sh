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

# Start a minimal echo backend using python3 (reliable, portable across all environments).
python3 -c '
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 18081))
s.listen(1)
conn, _ = s.accept()
while True:
    data = conn.recv(4096)
    if not data:
        break
    conn.sendall(data)
conn.close()
s.close()
' &
ECHO_PID=$!
sleep 0.3

# Start PulseProxy (single worker for the demo).
"$BINARY" --listen-port "$LISTEN_PORT" \
           --upstream-port "$UPSTREAM_PORT" \
           --workers 1 \
           --stats-interval 0 &
PP_PID=$!
sleep 0.5

echo "--- Sending 'Hello PulseProxy!' through the proxy ---"
RESPONSE=$(python3 -c '
import socket
s = socket.socket()
s.settimeout(3.0)
s.connect(("127.0.0.1", 18080))
s.sendall(b"Hello PulseProxy!\n")
data = s.recv(1024)
print(data.decode("utf-8", errors="replace"), end="")
s.close()
')
echo "Response: $RESPONSE"

kill "$PP_PID" "$ECHO_PID" 2>/dev/null
wait 2>/dev/null

if echo "$RESPONSE" | grep -q "Hello"; then
  echo "PASS"
else
  echo "FAIL: unexpected response"
  exit 1
fi
