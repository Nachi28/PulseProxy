// Tests for Config parsing.
#include "test_framework.hpp"
#include "pulseproxy/config.hpp"

using namespace pp;

PP_TEST(config_defaults) {
    Config cfg;
    PP_EXPECT_EQ(cfg.listen_port,   uint16_t(8080));
    PP_EXPECT_EQ(cfg.upstream_port, uint16_t(8081));
    PP_EXPECT_EQ(cfg.max_connections, 100000);
    PP_EXPECT(cfg.reuseport);
}

PP_TEST(config_parse_custom_ports) {
    Config cfg;
    const char* argv[] = { "pp", "--listen-port", "9090", "--upstream-port", "9091" };
    bool ok = Config::parse(5, const_cast<char**>(argv), cfg);
    PP_EXPECT(ok);
    PP_EXPECT_EQ(cfg.listen_port,   uint16_t(9090));
    PP_EXPECT_EQ(cfg.upstream_port, uint16_t(9091));
}

PP_TEST(config_no_reuseport_flag) {
    Config cfg;
    const char* argv[] = { "pp", "--no-reuseport" };
    bool ok = Config::parse(2, const_cast<char**>(argv), cfg);
    PP_EXPECT(ok);
    PP_EXPECT(!cfg.reuseport);
}

PP_TEST(config_workers_resolved) {
    Config cfg;
    const char* argv[] = { "pp" };
    bool ok = Config::parse(1, const_cast<char**>(argv), cfg);
    PP_EXPECT(ok);
    // After parse, worker_threads must be >= 1 (resolved from hardware_concurrency).
    PP_EXPECT_GE(cfg.worker_threads, 1);
}
