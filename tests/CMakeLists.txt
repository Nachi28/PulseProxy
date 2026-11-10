cmake_minimum_required(VERSION 3.18)

add_executable(pp_tests
  test_main.cpp
  test_arena.cpp
  test_socket_utils.cpp
  test_config.cpp
  test_e2e.cpp
)

target_link_libraries(pp_tests PRIVATE pp_core)

include(CTest)
add_test(NAME pulseproxy_tests COMMAND pp_tests)
