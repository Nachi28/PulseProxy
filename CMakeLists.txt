cmake_minimum_required(VERSION 3.18)
project(pulseproxy VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PP_BUILD_TESTS   "Build the PulseProxy test suite"       ON)
option(PP_BUILD_BENCH   "Build the PulseProxy benchmark binary" ON)
option(PP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ── Core library (Linux only — epoll, splice, SO_REUSEPORT) ─────────────────
add_library(pp_core
  src/config.cpp
  src/socket_utils.cpp
  src/memory_arena.cpp
  src/channel.cpp
  src/epoll_engine.cpp
)

target_include_directories(pp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_options(pp_core PRIVATE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow
  $<$<BOOL:${PP_WARNINGS_AS_ERRORS}>:-Werror>
)

# pthreads
find_package(Threads REQUIRED)
target_link_libraries(pp_core PUBLIC Threads::Threads)

# ── Main executable ──────────────────────────────────────────────────────────
add_executable(pulseproxy src/main.cpp)
target_link_libraries(pulseproxy PRIVATE pp_core)

# ── Benchmark ────────────────────────────────────────────────────────────────
if(PP_BUILD_BENCH)
  add_executable(pp_bench bench/main.cpp)
  target_link_libraries(pp_bench PRIVATE pp_core)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(PP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
