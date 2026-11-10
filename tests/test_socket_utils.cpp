// Tests for socket_utils — verify POSIX wrappers work correctly on Linux.
#include "test_framework.hpp"
#include "pulseproxy/socket_utils.hpp"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace pp;

PP_TEST(set_nonblocking_works) {
    int sv[2];
    PP_EXPECT(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    set_nonblocking(sv[0]);
    int flags = ::fcntl(sv[0], F_GETFL, 0);
    PP_EXPECT((flags & O_NONBLOCK) != 0);
    ::close(sv[0]);
    ::close(sv[1]);
}

PP_TEST(create_pipe_gives_nonblocking_fds) {
    Fd p[2];
    create_pipe(p);
    int f0 = ::fcntl(p[0], F_GETFL, 0);
    int f1 = ::fcntl(p[1], F_GETFL, 0);
    PP_EXPECT((f0 & O_NONBLOCK) != 0);
    PP_EXPECT((f1 & O_NONBLOCK) != 0);
    ::close(p[0]); ::close(p[1]);
}

PP_TEST(close_fd_idempotent) {
    int sv[2];
    PP_EXPECT(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    Fd fd = sv[0];
    ::close(sv[1]);
    close_fd(fd);
    PP_EXPECT_EQ(fd, INVALID_FD);
    // Calling again on INVALID_FD must not crash.
    close_fd(fd);
    PP_EXPECT_EQ(fd, INVALID_FD);
}

PP_TEST(create_listener_binds_on_loopback) {
    // Pick a high ephemeral port to avoid conflicts in CI.
    Fd lfd = create_listener("127.0.0.1", 19977, /*reuseport=*/false);
    PP_EXPECT(lfd != INVALID_FD);
    // Confirm the socket is non-blocking.
    int flags = ::fcntl(lfd, F_GETFL, 0);
    PP_EXPECT((flags & O_NONBLOCK) != 0);
    ::close(lfd);
}

PP_TEST(connect_nonblocking_reaches_listener) {
    // Create a listener, connect a non-blocking client, and confirm the
    // connect resolves (finish_connect returns true).
    Fd lfd = create_listener("127.0.0.1", 19978, false);
    PP_EXPECT(lfd != INVALID_FD);

    Fd cfd = connect_nonblocking("127.0.0.1", 19978);
    PP_EXPECT(cfd != INVALID_FD);

    // Wait for connect completion using poll with 1000ms timeout
    struct pollfd pfd{cfd, POLLOUT, 0};
    int prc = ::poll(&pfd, 1, 1000);
    PP_EXPECT(prc > 0);

    bool ok = finish_connect(cfd);
    PP_EXPECT(ok);

    sockaddr_in peer{};
    Fd afd = accept_nonblocking(lfd, peer);
    if (afd != INVALID_FD) ::close(afd);
    ::close(cfd);
    ::close(lfd);
}
