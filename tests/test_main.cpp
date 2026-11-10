// test_main.cpp — test runner entry point.
#include "test_framework.hpp"
#include <csignal>

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);
    return pp_test::run_all(argc, argv);
}
