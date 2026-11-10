#!/usr/bin/env bash
# build.sh — compile and test PulseProxy without cmake (useful when only g++ is available)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

INCS="-I${SCRIPT_DIR}/include"
FLAGS="-std=c++20 -O2 -Wall -Wextra -pthread"
OUT="/tmp/pp_build"
mkdir -p "$OUT"

echo "=== PulseProxy build (direct g++) ==="
echo "Source: $SCRIPT_DIR"

echo "[1/6] config.cpp"
g++ $FLAGS $INCS -c src/config.cpp        -o "$OUT/config.o"
echo "[2/6] socket_utils.cpp"
g++ $FLAGS $INCS -c src/socket_utils.cpp  -o "$OUT/socket_utils.o"
echo "[3/6] memory_arena.cpp"
g++ $FLAGS $INCS -c src/memory_arena.cpp  -o "$OUT/memory_arena.o"
echo "[4/6] channel.cpp"
g++ $FLAGS $INCS -c src/channel.cpp       -o "$OUT/channel.o"
echo "[5/6] epoll_engine.cpp"
g++ $FLAGS $INCS -c src/epoll_engine.cpp  -o "$OUT/epoll_engine.o"

OBJS="$OUT/config.o $OUT/socket_utils.o $OUT/memory_arena.o $OUT/channel.o $OUT/epoll_engine.o"

echo "[6/6] Link pulseproxy"
g++ $FLAGS $INCS src/main.cpp $OBJS -pthread -o "$OUT/pulseproxy"

echo ""
echo "=== Compiling tests ==="
g++ $FLAGS $INCS -I"${SCRIPT_DIR}/tests" \
    tests/test_main.cpp \
    tests/test_arena.cpp \
    tests/test_socket_utils.cpp \
    tests/test_config.cpp \
    tests/test_e2e.cpp \
    $OBJS -pthread -o "$OUT/pp_tests"

echo ""
echo "=== Running tests ==="
"$OUT/pp_tests"

echo ""
echo "=== Compiling benchmark ==="
g++ $FLAGS $INCS bench/main.cpp $OBJS -pthread -o "$OUT/pp_bench"

echo ""
echo "============================================"
echo " Build succeeded."
echo " Binaries:"
echo "   $OUT/pulseproxy"
echo "   $OUT/pp_tests"
echo "   $OUT/pp_bench"
echo ""
echo " Run the proxy:"
echo "   $OUT/pulseproxy --listen-port 8080 --upstream-port 8081"
echo "============================================"
