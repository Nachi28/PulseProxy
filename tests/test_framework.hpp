// Minimal zero-dependency test runner.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace pp_test {

struct TestCase {
    std::string              name;
    std::function<void()>    fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

#define PP_TEST(name)                                        \
    static void _pp_test_##name();                           \
    static ::pp_test::Registrar _pp_reg_##name(#name,       \
                                _pp_test_##name);            \
    static void _pp_test_##name()

#define PP_EXPECT(expr)                                       \
    do {                                                      \
        if (!(expr)) {                                        \
            std::fprintf(stderr, "  FAIL  %s:%d  %s\n",      \
                         __FILE__, __LINE__, #expr);          \
            std::exit(1);                                     \
        }                                                     \
    } while(0)

#define PP_EXPECT_EQ(a, b) PP_EXPECT((a) == (b))
#define PP_EXPECT_NE(a, b) PP_EXPECT((a) != (b))
#define PP_EXPECT_GE(a, b) PP_EXPECT((a) >= (b))

inline int run_all(int argc, char** argv) {
    (void)argc; (void)argv;
    int passed = 0, failed = 0;
    for (auto& tc : registry()) {
        std::fprintf(stderr, "[ RUN  ] %s\n", tc.name.c_str());
        try {
            tc.fn();
            std::fprintf(stderr, "[ PASS ] %s\n", tc.name.c_str());
            ++passed;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[ FAIL ] %s  exception: %s\n",
                         tc.name.c_str(), e.what());
            ++failed;
        }
    }
    std::fprintf(stderr, "\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}

} // namespace pp_test
