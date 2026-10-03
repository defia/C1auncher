/* Standalone white-box PTY/fault tests. No Makefile changes are needed:
 * cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -Isrc
 *    tests/test_terminal_idle_faults.c src/platform/liveness.c
 *    src/platform/app_lease.c -o build/host-terminal-idle-faults-tests
 */
#define _GNU_SOURCE 1
#define _XOPEN_SOURCE 600
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static enum {
    FAULT_NONE, FAULT_PROC_DIRECTORY, FAULT_PROC_STAT, FAULT_MALFORMED_STAT,
    FAULT_STOP_WAIT, FAULT_STOP_CLOCK, FAULT_MISSING_PROMPT,
    FAULT_ONCE_PROC_STAT, FAULT_ONCE_STALE_REPLY, FAULT_ONCE_WRONG_SENDER,
    FAULT_ONCE_UNKNOWN_REPLY, FAULT_IMAGE_CREATE, FAULT_IMAGE_WRITE,
    FAULT_IMAGE_MODE, FAULT_IMAGE_OPEN, FAULT_IMAGE_INVALID
} fault;
static unsigned int wait_calls, injected;
#include "services/terminal_idle_protocol.h"

static DIR *idle_test_opendir(const char *path)
{
    if (fault == FAULT_PROC_DIRECTORY && strcmp(path, "/proc") == 0) {
        errno = EACCES;
        return NULL;
    }
    return opendir(path);
}

static FILE *idle_test_fopen(const char *path, const char *mode)
{
    if (strncmp(path, "/proc/", 6U) == 0 && strstr(path, "/stat") != NULL) {
        if (fault == FAULT_PROC_STAT || (fault == FAULT_ONCE_PROC_STAT && injected++ == 0U)) {
            errno = EIO;
            return NULL;
        }
        if (fault == FAULT_MALFORMED_STAT) {
            static char invalid[] = "unknown proc format\n";
            return fmemopen(invalid, sizeof(invalid) - 1U, "r");
        }
    }
    return fopen(path, mode);
}

static int idle_test_waitid(idtype_t type, id_t id, siginfo_t *info, int options)
{
    ++wait_calls;
    if (fault == FAULT_STOP_WAIT && wait_calls % 2U == 0U) { errno = EIO; return -1; }
    return waitid(type, id, info, options);
}

static int idle_test_clock_gettime(clockid_t clock, struct timespec *value)
{
    if (fault == FAULT_STOP_CLOCK && wait_calls >= 2U) { errno = EIO; return -1; }
    return clock_gettime(clock, value);
}

static int idle_test_setenv(const char *name, const char *value, int overwrite)
{
    /* An exec fallback/unknown startup has no trusted prompt notification. */
    if (fault == FAULT_MISSING_PROMPT && strcmp(name, "PROMPT_COMMAND") == 0) value = ":";
    return setenv(name, value, overwrite);
}

static ssize_t idle_test_recvmsg(int descriptor, struct msghdr *message, int flags)
{
    ssize_t result = recvmsg(descriptor, message, flags);
    if (result == (ssize_t)sizeof(c1_terminal_idle_message) &&
        fault >= FAULT_ONCE_STALE_REPLY && fault <= FAULT_ONCE_UNKNOWN_REPLY && injected++ == 0U) {
        c1_terminal_idle_message *proof = message->msg_iov[0].iov_base;
        if (fault == FAULT_ONCE_STALE_REPLY) --proof->token;
        if (fault == FAULT_ONCE_UNKNOWN_REPLY) proof->magic = 0U;
        if (fault == FAULT_ONCE_WRONG_SENDER) {
            struct cmsghdr *header;
            for (header = CMSG_FIRSTHDR(message); header != NULL; header = CMSG_NXTHDR(message, header)) {
                if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_CREDENTIALS) {
                    struct ucred sender;
                    memcpy(&sender, CMSG_DATA(header), sizeof(sender));
                    ++sender.pid;
                    memcpy(CMSG_DATA(header), &sender, sizeof(sender));
                }
            }
        }
    }
    return result;
}

#ifdef C1_TERMINAL_IDLE_BLOB_HEADER
static int image_writer = -1;
static int idle_test_mkstemp(char *path)
{
    if (fault == FAULT_IMAGE_CREATE) { errno = ENOSPC; return -1; }
    return image_writer = mkstemp(path);
}
static ssize_t idle_test_write(int fd, const void *data, size_t size)
{
    if (fd == image_writer) {
        if (fault == FAULT_IMAGE_WRITE) { errno = ENOSPC; return -1; }
        if (fault == FAULT_IMAGE_INVALID) {
            static const unsigned char invalid[1] = {0};
            return write(fd, invalid, 1U);
        }
    }
    return write(fd, data, size);
}
static int idle_test_fchmod(int fd, mode_t mode)
{
    if (fd == image_writer && fault == FAULT_IMAGE_MODE) { errno = EPERM; return -1; }
    return fchmod(fd, mode);
}
static int idle_test_open(const char *path, int flags)
{
    if (fault == FAULT_IMAGE_OPEN && strncmp(path, "/proc/self/fd/", 14U) == 0) {
        errno = EACCES;
        return -1;
    }
    return open(path, flags);
}
#define mkstemp idle_test_mkstemp
#define write idle_test_write
#define fchmod idle_test_fchmod
#define open idle_test_open
#endif
#define opendir idle_test_opendir
#define fopen idle_test_fopen
#define waitid idle_test_waitid
#define clock_gettime idle_test_clock_gettime
#define setenv idle_test_setenv
#define recvmsg idle_test_recvmsg
#undef _XOPEN_SOURCE
#include "../src/services/terminal.c"
#undef opendir
#undef fopen
#undef waitid
#undef clock_gettime
#undef setenv
#undef recvmsg
#undef mkstemp
#undef write
#undef fchmod
#undef open

/* Reuse real PTY helpers and the complete existing lifecycle suite. */
#define main lifecycle_suite_main
#include "test_terminal_lifecycle.c"
#undef main

static void test_idle_fault(int which)
{
    c1_terminal_session session;
    c1_status written = C1_STATUS_UNAVAILABLE;
    const char command[] = "printf 'AFTER_%s\\n' DENIAL\r";
    int64_t deadline;
    fault = which;
    wait_calls = 0U;
    c1_terminal_init(&session);
    expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "start fault-injected real Bash");
    expect(wait_output(&session, geteuid() == 0 ? "# " : "$ "), "fault fixture reached prompt");
    expect_protected(&session, "unknown proc/wait/clock/prompt state fails closed");
    expect(process_state(session.shell_pid) != 'T', "failed verification always resumes its own freeze");
    deadline = now_ms() + 2000;
    while (written == C1_STATUS_UNAVAILABLE && now_ms() < deadline) {
        written = c1_terminal_write(&session, command, sizeof(command) - 1U);
        pause_tick();
    }
    expect(written == C1_STATUS_OK, "denied asynchronous request permits later input");
    expect(wait_output(&session, "AFTER_DENIAL"), "shell still executes after failed verification");
    c1_terminal_stop(&session);
    fault = FAULT_NONE;
}

static void test_delayed_idle_request(void)
{
    c1_terminal_session session;
    const char command[] = "printf 'DELAYED_%s\\n' INPUT\r";
    int64_t deadline;
    c1_status started;
    fault = FAULT_MISSING_PROMPT;
    c1_terminal_init(&session);
    started = c1_terminal_start(&session, 49, 19);
    expect(started == C1_STATUS_OK, "start delayed-supervisor input fixture");
    if (started != C1_STATUS_OK) { fault = FAULT_NONE; return; }
    expect(wait_output(&session, geteuid() == 0 ? "# " : "$ "), "delayed fixture reached prompt");
    expect(kill(session.child_pid, SIGSTOP) == 0, "delay only the fixture's owned supervisor");
    deadline = now_ms() + 1000;
    while (process_state(session.child_pid) != 'T' && now_ms() < deadline) pause_tick();
    expect(process_state(session.child_pid) == 'T', "supervisor delay is deterministic");
    expect(!c1_terminal_close_idle(&session) && session.idle_close_pending,
           "close request remains pending while supervisor cannot answer");
    for (unsigned int i = 0U; i < 15U; ++i) pause_tick();
    expect(!c1_terminal_close_idle(&session) && session.idle_close_pending,
           "a UI's 100ms budget cannot guarantee a completed reply");
    expect(c1_terminal_write(&session, command, sizeof(command) - 1U) == C1_STATUS_UNAVAILABLE,
           "delayed close reports retryable input backpressure, not successful delivery");
    expect(!session.input_seen && session.pending_length == 0U,
           "rejected input cannot enter a shell that might still be closed");
    expect(kill(session.child_pid, SIGCONT) == 0, "resume the delayed supervisor");
    deadline = now_ms() + 3000;
    while (session.idle_close_pending && now_ms() < deadline) {
        expect(!c1_terminal_close_idle(&session), "missing proof rejects delayed close");
        pause_tick();
    }
    expect(!session.idle_close_pending && c1_terminal_is_running(&session),
           "denial ends this attempt without immediately submitting another request");
    expect(c1_terminal_write(&session, command, sizeof(command) - 1U) == C1_STATUS_OK,
           "caller can retry the exact rejected input after denial");
    expect(wait_output(&session, "DELAYED_INPUT"), "retried input executes in the preserved shell");
    expect(session.input_seen, "retried user input is recorded without becoming a permanent veto");
    c1_terminal_stop(&session);
    fault = FAULT_NONE;
}

static void test_supervisor_descendants(void)
{
    static const char *commands[] = {
        "sleep 30 & printf 'TREE_%s\\n' READY\r",
        "sleep 30 & kill -STOP $!; printf 'TREE_%s\\n' READY\r",
        "setsid sleep 30 & disown; printf 'TREE_%s\\n' READY\r",
        "(setsid sleep 30 </dev/null >/dev/null 2>&1 &) & wait; printf 'TREE_%s\\n' READY\r"
    };
    for (size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        c1_terminal_session session;
        c1_terminal_init(&session);
        expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "start supervisor tree fixture");
        expect(wait_output(&session, geteuid() == 0 ? "# " : "$ "), "tree fixture reached prompt");
        expect(c1_terminal_write(&session, commands[i], strlen(commands[i])) == C1_STATUS_OK,
               "create background/stopped/detached/adopted child through real PTY");
        expect(wait_output(&session, "TREE_READY"), "descendant fixture is ready");
        /* The real native hook proves an empty prompt, but the supervisor's
         * independent descendant proof must still reject every living job. */
        expect_protected(&session, "supervisor rejects every descendant even if caller claims idle");
        expect(process_state(session.shell_pid) != 'T', "nonempty tree check resumes its shell");
        c1_terminal_stop(&session);
    }
}

static void test_recoverable_fault(int which)
{
    c1_terminal_session session;
    int64_t deadline;
    fault = which;
    injected = wait_calls = 0U;
    c1_terminal_init(&session);
    expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "start transient-fault shell");
    expect(wait_output(&session, geteuid() == 0 ? "# " : "$ "), "transient-fault shell reached prompt");
    expect(!c1_terminal_close_idle(&session) && session.idle_close_pending, "send first fault-injected request");
    deadline = now_ms() + 2000;
    while (session.idle_close_pending && now_ms() < deadline) {
        expect(!c1_terminal_close_idle(&session), "stale/foreign/unknown proof or proc failure cannot close shell");
        pause_tick();
    }
    expect(!session.idle_close_pending && c1_terminal_is_running(&session), "failed request is denied, not latched");
    expect(process_state(session.shell_pid) != 'T', "failed proof returns shell to its original running state");
    expect(retry_terminal_write(&session, "printf 'RECOVER_%s\\n' ALIVE\r") == C1_STATUS_OK,
           "released proof did not strand Readline waiting for its supervisor");
    expect(wait_output(&session, "RECOVER_ALIVE"), "shell can execute after transient fault");
    expect(wait_idle_close(&session), "a new challenge succeeds after the one-shot fault clears");
    c1_terminal_stop(&session);
    fault = FAULT_NONE;
}

static void test_missing_helper_recovery(void)
{
#ifdef C1_TERMINAL_IDLE_BLOB_HEADER
    c1_terminal_session session;
    const char *configured = getenv(C1_TERMINAL_IDLE_HELPER_ENV);
    char *saved = configured != NULL ? strdup(configured) : NULL;
    expect(setenv(C1_TERMINAL_IDLE_HELPER_ENV, "/nonexistent/ignored-helper", 1) == 0,
           "production ignores inherited helper path");
    c1_terminal_init(&session);
    expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "embedded module needs no sidecar");
    expect(wait_idle_close(&session), "embedded module overrides missing inherited helper");
    c1_terminal_stop(&session);
    if (saved != NULL) { (void)setenv(C1_TERMINAL_IDLE_HELPER_ENV, saved, 1); free(saved); }
    else (void)unsetenv(C1_TERMINAL_IDLE_HELPER_ENV);
#else
    c1_terminal_session session;
    const char *configured = getenv(C1_TERMINAL_IDLE_HELPER_ENV);
    char *saved = configured != NULL ? strdup(configured) : NULL;
    char helper[4096], command[4608];
    ssize_t count;
    (void)unsetenv(C1_TERMINAL_IDLE_HELPER_ENV);
    c1_terminal_init(&session);
    expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK,
           "standalone without explicit helper still starts Bash");
    expect(wait_output(&session, geteuid() == 0 ? "# " : "$ "), "unconfigured shell reaches prompt");
    expect_protected(&session, "standalone missing helper environment fails closed");
    c1_terminal_stop(&session);
    count = readlink("/proc/self/exe", helper, sizeof(helper) - 1U);
    char *slash;
    expect(count > 0, "locate real helper for same-session recovery");
    if (count <= 0) { free(saved); return; }
    helper[count] = '\0';
    slash = strrchr(helper, '/');
    if (slash == NULL) { free(saved); return; }
    *slash = '\0';
    snprintf(command, sizeof(command),
        "export C1_TERMINAL_IDLE_HELPER='%s%s'; echo helper-restored\r",
        saved != NULL ? saved : helper, saved != NULL ? "" : "/c1-terminal-idle.so");
    expect(setenv(C1_TERMINAL_IDLE_HELPER_ENV, "/nonexistent/c1-terminal-idle.so", 1) == 0,
           "temporarily remove native helper from startup");
    c1_terminal_init(&session);
    expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "missing helper does not break terminal startup");
    if (saved != NULL) (void)setenv(C1_TERMINAL_IDLE_HELPER_ENV, saved, 1);
    else (void)unsetenv(C1_TERMINAL_IDLE_HELPER_ENV);
    expect(wait_output(&session, geteuid() == 0 ? "# " : "$ "), "ordinary Bash still displays prompt without helper");
    expect_protected(&session, "missing native state proof safely denies automatic close");
    expect(retry_terminal_write(&session, command) == C1_STATUS_OK, "repair helper in the same running shell");
    expect(wait_idle_close(&session), "missing helper is recoverable, not a permanent input veto");
    c1_terminal_stop(&session);
    free(saved);
#endif
}

#ifdef C1_TERMINAL_IDLE_BLOB_HEADER
static void test_image_fd(void)
{
    struct stat info;
    unsigned char bytes[sizeof(c1_terminal_idle_image)];
    int fd = embedded_idle_helper();
    expect(fd >= 0, "create embedded module without a persistent pathname");
    if (fd < 0) return;
    expect(fstat(fd, &info) == 0 && info.st_nlink == 0 && (info.st_mode & 0777) == 0400,
           "image is unlinked and owner-read-only before exec");
    expect((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY,
           "only a read-only image descriptor survives startup");
    expect(read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes) &&
           memcmp(bytes, c1_terminal_idle_image, sizeof(bytes)) == 0,
           "complete embedded ELF is written without truncation");
    close(fd);
    image_writer = -1;
}
#endif

int main(int argc, char **argv)
{
    if (argc > 1) return fixture(argc, argv);
#ifdef C1_TERMINAL_IDLE_BLOB_HEADER
    test_image_fd();
#endif
    if (lifecycle_suite_main(argc, argv) != 0) return 1;
    for (int which = FAULT_PROC_DIRECTORY; which <= FAULT_MISSING_PROMPT; ++which)
        test_idle_fault(which);
    test_delayed_idle_request();
    for (int which = FAULT_ONCE_PROC_STAT; which <= FAULT_ONCE_UNKNOWN_REPLY; ++which)
        test_recoverable_fault(which);
    test_supervisor_descendants();
    test_missing_helper_recovery();
#ifdef C1_TERMINAL_IDLE_BLOB_HEADER
    for (int which = FAULT_IMAGE_CREATE; which <= FAULT_IMAGE_INVALID; ++which)
        test_idle_fault(which);
#endif
    if (failures) return 1;
    puts("all terminal idle fault tests passed");
    return 0;
}
