/* Host-only fault injection. Linked only by test_shutdown_supervision.py.
 * No system power commands, devices, or production feature switches. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int inject(const char *setting, const char *name, int error)
{
    const char *mode = getenv(setting), *root = getenv("FIXTURE_ROOT");
    char path[4096];
    int fd;
    if (mode == NULL || root == NULL) return 0;
    if (snprintf(path, sizeof(path), "%s/%s-attempts", root, name) >= (int)sizeof(path)) abort();
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0 || write(fd, "attempt\n", 8) != 8) abort();
    close(fd);
    if (strstr(mode, "once") != NULL) {
        if (snprintf(path, sizeof(path), "%s/%s-injected", root, name) >= (int)sizeof(path)) abort();
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) return 0;
            abort();
        }
        close(fd);
    }
    errno = strncmp(mode, "eintr", 5) == 0 ? EINTR : error;
    return -1;
}

#ifdef C1_TEST_PAIR_FAULT
int __real_socketpair(int domain, int type, int protocol, int descriptors[2]);
int __wrap_socketpair(int domain, int type, int protocol, int descriptors[2])
{
    if (inject("FIXTURE_PAIR_FAILURE", "pair", EMFILE) != 0) return -1;
    return __real_socketpair(domain, type, protocol, descriptors);
}
#else
int __real_getsockopt(int fd, int level, int option, void *value, socklen_t *size);
int __wrap_getsockopt(int fd, int level, int option, void *value, socklen_t *size)
{
    if (level == SOL_SOCKET && option == SO_TYPE &&
        inject("FIXTURE_INIT_FAILURE", "init", EIO) != 0) return -1;
    return __real_getsockopt(fd, level, option, value, size);
}
#endif
