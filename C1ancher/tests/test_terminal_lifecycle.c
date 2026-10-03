#define _GNU_SOURCE 1
#include "services/terminal.h"
#include "platform/app_lease.h"
#include "platform/liveness.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int failures;
static void expect(bool okay, const char *message)
{
    if (!okay) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
static int64_t now_ms(void)
{
    struct timespec value;
    (void)clock_gettime(CLOCK_MONOTONIC, &value);
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}
static void pause_tick(void)
{
    struct timespec delay = {0, 10000000L};
    (void)nanosleep(&delay, NULL);
}
static pid_t read_pid(const char *path)
{
    FILE *file = fopen(path, "r");
    long pid = -1;
    if (file != NULL) { if (fscanf(file, "%ld", &pid) != 1) pid = -1; fclose(file); }
    return (pid_t)pid;
}

/* Session root creates a foreground group that ignores HUP, or a detached
 * descendant. The session root may exit before its child; neither is a reason
 * to skip cleanup. Its lease descriptor intentionally survives exec. */
static int fixture(int argc, char **argv)
{
    int ready[2];
    pid_t child;
    if (argc < 5) return 126;
    if (strcmp(argv[1], "--hold") == 0) {
        FILE *file = fopen(argv[3], "w");
        if (file == NULL) return 126;
        fprintf(file, "%ld\n", (long)getpid()); fclose(file);
        for (;;) pause();
    }
    if (pipe(ready) != 0) return 126;
    child = fork();
    if (child < 0) return 126;
    if (child == 0) {
        int lease;
        close(ready[0]);
        (void)signal(SIGHUP, SIG_IGN);
        (void)signal(SIGTERM, SIG_IGN);
        (void)signal(SIGTTOU, SIG_IGN);
        if (strcmp(argv[1], "--detached") == 0) {
            if (setsid() < 0) _exit(126);
        } else {
            if (setpgid(0, 0) != 0 || tcsetpgrp(STDIN_FILENO, getpid()) != 0) _exit(126);
        }
        lease = c1_app_run_acquire_at(argv[2]);
        if (lease < 0 || (fcntl(lease, F_GETFD) & FD_CLOEXEC)) _exit(126);
        if (write(ready[1], "R", 1U) != 1) _exit(126);
        close(ready[1]);
        execl(argv[4], argv[4], "--hold", argv[2], argv[3], argv[4], (char *)NULL);
        _exit(127);
    }
    close(ready[1]);
    {
        char byte;
        if (read(ready[0], &byte, 1U) != 1) return 126;
    }
    close(ready[0]);
    /* Wait for exec and the identity marker, not merely the fork. */
    while (read_pid(argv[3]) <= 0) pause_tick();
    if (strcmp(argv[1], "--shell-first") == 0 || strcmp(argv[1], "--detached") == 0)
        return 0;
    for (;;) pause();
}

static void test_cleanup(const char *self, const char *mode)
{
    char root[] = "/tmp/c1-terminal-lifecycle-XXXXXX";
    char lock[256], pidfile[256];
    char *arguments[] = {(char *)self, (char *)mode, lock, pidfile, (char *)self, NULL};
    c1_terminal_session session;
    int64_t deadline;
    pid_t descendant = -1;
    int lease;
    expect(mkdtemp(root) != NULL, "create isolated cleanup fixture");
    snprintf(lock, sizeof(lock), "%s/lock", root);
    snprintf(pidfile, sizeof(pidfile), "%s/pid", root);
    c1_terminal_init(&session);
    expect(c1_terminal_start_exec(&session, 49, 19, self, arguments) == C1_STATUS_OK,
           "start dedicated executable session");
    deadline = now_ms() + 3000;
    while ((descendant = read_pid(pidfile)) <= 0 && now_ms() < deadline) pause_tick();
    expect(descendant > 0, "foreground or detached descendant reached exec");
    if (strcmp(mode, "--foreground") != 0) {
        while (c1_terminal_is_running(&session) && now_ms() < deadline) pause_tick();
        expect(!c1_terminal_is_running(&session), "shell exit completes bounded descendant cleanup");
    }
    c1_terminal_stop(&session);
    expect(descendant > 0 && kill(descendant, 0) != 0 && errno == ESRCH,
           "session cleanup reaps HUP-ignoring descendants even after shell exits");
    lease = c1_app_run_acquire_at(lock);
    expect(lease >= 0, "inherited run lock released after descendant exec and cleanup");
    c1_app_lease_release(lease);
    unlink(lock); unlink(pidfile); rmdir(root);
}

static void test_parent_crash(const char *self)
{
    char root[] = "/tmp/c1-terminal-owner-XXXXXX";
    char lock[256], pidfile[256];
    char *arguments[] = {(char *)self, "--foreground", lock, pidfile, (char *)self, NULL};
    int report[2];
    pid_t owner, supervisor = -1, descendant;
    int64_t deadline;
    bool reaped = false;
    expect(mkdtemp(root) != NULL, "create crashed terminal owner fixture");
    snprintf(lock, sizeof(lock), "%s/lock", root);
    snprintf(pidfile, sizeof(pidfile), "%s/pid", root);
    expect(pipe(report) == 0, "create supervisor identity pipe");
    owner = fork();
    if (owner == 0) {
        c1_terminal_session session;
        close(report[0]);
        c1_terminal_init(&session);
        if (c1_terminal_start_exec(&session, 49, 19, self, arguments) != C1_STATUS_OK) _exit(126);
        if (write(report[1], &session.child_pid, sizeof(session.child_pid)) !=
            (ssize_t)sizeof(session.child_pid)) _exit(126);
        deadline = now_ms() + 3000;
        while (read_pid(pidfile) <= 0 && now_ms() < deadline) pause_tick();
        _exit(1); /* Deliberately bypass c1_terminal_stop, as a crashed UI does. */
    }
    close(report[1]);
    expect(read(report[0], &supervisor, sizeof(supervisor)) == (ssize_t)sizeof(supervisor),
           "record owned session supervisor before UI crash");
    close(report[0]);
    waitpid(owner, NULL, 0);
    descendant = read_pid(pidfile);
    deadline = now_ms() + 3000;
    while (supervisor > 0 && now_ms() < deadline) {
        if (waitpid(supervisor, NULL, WNOHANG) == supervisor) { reaped = true; break; }
        pause_tick();
    }
    expect(reaped, "anonymous control pipe detects UI death without a service");
    expect(descendant > 0 && kill(descendant, 0) < 0 && errno == ESRCH,
           "crashed UI cannot leave HUP-ignoring terminal descendants");
    expect(!c1_app_run_active_at(lock), "UI crash releases descendants' inherited application lock");
    unlink(lock); unlink(pidfile); rmdir(root);
}

static void test_busy_terminal_app(void)
{
    c1_terminal_session user, app;
    char *arguments[] = {"/bin/sh", "-c", "printf APP_ONLY; sleep .15", NULL};
    const char command[] = "printf OLD_NEOFETCH; sleep 30 & sleep 30\r";
    char output[512] = "";
    size_t used = 0;
    int64_t deadline = now_ms() + 3000;
    pid_t user_supervisor;
    c1_terminal_init(&user); c1_terminal_init(&app);
    expect(c1_terminal_start(&user, 49, 19) == C1_STATUS_OK, "start normal user shell");
    while (!c1_terminal_shell_is_foreground(&user) && now_ms() < deadline) pause_tick();
    expect(c1_terminal_write(&user, command, sizeof(command)-1U) == C1_STATUS_OK,
           "normal user terminal has existing foreground and background work");
    while (c1_terminal_shell_is_foreground(&user) && now_ms() < deadline) pause_tick();
    expect(!c1_terminal_shell_is_foreground(&user), "normal terminal is busy");
    user_supervisor = user.child_pid;
    expect(c1_terminal_start_exec(&app, 49, 19, arguments[0], arguments) == C1_STATUS_OK,
           "busy shell cannot drop a dedicated APP request");
    while (c1_terminal_is_running(&app) && now_ms() < deadline) {
        ssize_t count = c1_terminal_read(&app, output + used, sizeof(output)-used-1U);
        if (count > 0) { used += (size_t)count; output[used] = '\0'; }
        pause_tick();
    }
    expect(strstr(output, "APP_ONLY") != NULL && strstr(output, "OLD_NEOFETCH") == NULL,
           "APP output is isolated from the old normal terminal screen");
    expect(!c1_terminal_is_running(&app), "dedicated app exits instead of returning to a shell");
    c1_terminal_stop(&app);
    expect(c1_terminal_is_running(&user) && user.child_pid == user_supervisor &&
           kill(user_supervisor, 0) == 0, "closing APP preserves the busy user terminal");
    c1_terminal_stop(&user);
}

static void drain_terminal(c1_terminal_session *session)
{
    char buffer[2048];
    for (unsigned int i = 0; i < 32U; ++i)
        if (c1_terminal_read(session, buffer, sizeof(buffer)) <= 0) break;
}

static bool wait_output(c1_terminal_session *session, const char *needle)
{
    char output[8192] = "";
    size_t used = 0U;
    int64_t deadline = now_ms() + 3000;
    while (now_ms() < deadline) {
        ssize_t count = c1_terminal_read(session, output + used, sizeof(output) - used - 1U);
        if (count > 0) {
            used += (size_t)count;
            output[used] = '\0';
            if (strstr(output, needle) != NULL) return true;
            if (used > sizeof(output) / 2U) {
                memmove(output, output + used / 2U, used - used / 2U);
                used -= used / 2U;
            }
        }
        pause_tick();
    }
    fprintf(stderr, "waiting for %s, received: %s\n", needle, output);
    return false;
}

static char process_state(pid_t pid)
{
    char path[64], data[2048], state = '?';
    FILE *file;
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    file = fopen(path, "r");
    if (file != NULL) {
        if (fgets(data, sizeof(data), file) != NULL) {
            char *end = strrchr(data, ')');
            if (end != NULL) (void)sscanf(end + 1, " %c", &state);
        }
        fclose(file);
    }
    return state;
}

static bool wait_idle_close(c1_terminal_session *session)
{
    int64_t deadline = now_ms() + 3000;
    while (now_ms() < deadline) {
        int64_t before = now_ms();
        bool closed = c1_terminal_close_idle(session);
        expect(now_ms() - before < 100, "idle-close API never waits for the supervisor");
        if (closed) return true;
        drain_terminal(session);
        pause_tick();
    }
    return false;
}

static void expect_no_image_fd(pid_t shell)
{
    char path[64], target[256];
    struct dirent *entry;
    DIR *fds;
    snprintf(path, sizeof(path), "/proc/%ld/fd", (long)shell);
    fds = opendir(path);
    expect(fds != NULL, "inspect shell descriptors after helper installation");
    if (fds == NULL) return;
    while ((entry = readdir(fds)) != NULL) {
        ssize_t count = readlinkat(dirfd(fds), entry->d_name, target, sizeof(target) - 1U);
        if (count < 0) continue;
        target[count] = '\0';
        expect(strstr(target, "c1-terminal-idle-") == NULL,
               "loaded helper leaves no open image descriptor in Bash");
    }
    closedir(fds);
}

static void test_idle_shell(void)
{
    c1_terminal_session session;
    c1_terminal_init(&session);
    expect(c1_terminal_close_idle(&session), "an initialized empty session is safe");
    expect(!c1_terminal_close_idle(NULL), "unknown session is protected");
    for (unsigned int i = 0; i < 8U; ++i) {
        pid_t shell;
        expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "start successive pristine Bash");
        shell = session.shell_pid;
        /* No UI PTY reads are required: Home may retain a hidden terminal. */
        expect(wait_idle_close(&session), "pristine prompt shell can be closed and reaped");
        expect(session.child_pid <= 0 && session.master_fd < 0 && session.control_fd < 0,
               "successful idle close has released every session resource");
        expect(kill(shell, 0) < 0 && errno == ESRCH, "idle shell is reaped, not merely signalled");
    }
    c1_terminal_stop(&session);
}

static void expect_protected(c1_terminal_session *session, const char *description)
{
    pid_t supervisor = session->child_pid, shell = session->shell_pid;
    for (unsigned int i = 0U; i < 12U; ++i) {
        expect(!c1_terminal_close_idle(session), description);
        drain_terminal(session);
        pause_tick();
    }
    /* Finish the last asynchronous denial before examining SIGSTOP state.
     * Under instrumentation the supervisor may still be in its bounded scan. */
    {
        int64_t deadline = now_ms() + 2000;
        while (session->idle_close_pending && now_ms() < deadline) {
            expect(!c1_terminal_close_idle(session), description);
            pause_tick();
        }
        expect(!session->idle_close_pending, "protected attempt receives a bounded denial");
    }
    expect(c1_terminal_is_running(session) && session->child_pid == supervisor &&
           kill(shell, 0) == 0, "denied idle close preserves the original session");
}

static void test_idle_input_protection(void)
{
    static const struct {
        const char *command;
        const char *ready;
    } cases[] = {
        {"printf 'R_%s\\n' FOREGROUND; sleep 30\r", "R_FOREGROUND"},
        {"sleep 30 & printf 'R_%s\\n' BACKGROUND\r", "R_BACKGROUND"},
        {"sleep 30 & kill -STOP $!; printf 'R_%s\\n' STOPPED\r", "R_STOPPED"},
        {"setsid sleep 30 & disown; printf 'R_%s\\n' DETACHED\r", "R_DETACHED"},
        {"printf 'R_%s\\n' READ; read -r answer\r", "R_READ"},
        {"printf 'R_%s\\n' READLINE; read -e -r answer\r", "R_READLINE"},
        {"printf 'R_%s\\n' LOOP; while :; do :; done\r", "R_LOOP"},
        {"ls\rpartial", NULL},
        {"printf 'R_%s\\n' PASTED\rpartial", "R_PASTED"},
        {"printf ", NULL},
        {"history -s 'echo history'; printf 'R_%s\\n' HISTORY\r\033[A", "R_HISTORY"},
        {"echo 'unfinished\r", NULL},
        {"if true; then\r", NULL},
        {"\033[200~echo pasted\npartial\033[201~", NULL},
        {"\033[200~", NULL},
        {"\026", NULL},
        {"\022", NULL}
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        c1_terminal_session session;
        c1_terminal_init(&session);
        expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "start protected shell fixture");
        expect(wait_output(&session, geteuid() == 0 ? "# " : "$ "), "shell reached its initial prompt");
        expect_no_image_fd(session.shell_pid);
        expect(c1_terminal_write(&session, cases[i].command, strlen(cases[i].command)) == C1_STATUS_OK,
               "accept shell command or partial Readline input");
        if (cases[i].ready != NULL)
            expect(wait_output(&session, cases[i].ready), "real PTY command reached its protected state");
        expect_protected(&session, "user input, foreground/background work and builtins stay protected");
        expect(process_state(session.shell_pid) != 'T', "denied check does not leave shell paused");
        c1_terminal_stop(&session);
    }
}

static c1_status retry_terminal_write(c1_terminal_session *session, const char *command)
{
    int64_t deadline = now_ms() + 3000;
    c1_status result;
    do {
        result = c1_terminal_write(session, command, strlen(command));
        if (result != C1_STATUS_UNAVAILABLE) return result;
        pause_tick();
    } while (now_ms() < deadline);
    return result;
}

static void test_commands_become_idle(void)
{
    static const char *commands[] = {
        "ls\r", "pwd\r", "echo ordinary\r",
        "ls\rpwd\recho multi-one\r",
        "printf '%s\\n' pipeline | /bin/cat\r",
        "for item in one two; do echo $item; done\r"
    };
    for (size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        c1_terminal_session session;
        c1_terminal_init(&session);
        expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "start command-completion fixture");
        /* Deliberately write before reading even the initial prompt: delayed
         * startup/prompt text must neither prove busy nor prove idle. */
        expect(c1_terminal_write(&session, commands[i], strlen(commands[i])) == C1_STATUS_OK,
               "accept ordinary command or multiple pasted commands");
        expect(wait_idle_close(&session), "completed ordinary commands regain idle-close eligibility");
        c1_terminal_stop(&session);
    }
    for (unsigned int round = 0U; round < 3U; ++round) {
        c1_terminal_session session;
        c1_terminal_init(&session);
        expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "start repeated multi-command session");
        for (unsigned int batch = 0U; batch < 2U; ++batch) {
            const char *command = batch == 0U ?
                "ls\rpwd\rprintf 'BATCH_%s\\n' ONE\r" :
                "echo next\rpwd\rprintf 'BATCH_%s\\n' TWO\r";
            expect(retry_terminal_write(&session, command) == C1_STATUS_OK, "write next complete command batch");
            expect(wait_output(&session, batch == 0U ? "BATCH_ONE" : "BATCH_TWO"),
                   "both successive batches actually execute");
        }
        expect(wait_idle_close(&session), "two rounds of commands close after the last empty prompt");
        c1_terminal_stop(&session);
    }
}

static void test_busy_then_idle(void)
{
    static const struct { const char *busy; const char *finish; const char *ready; } cases[] = {
        {"echo partial", "\025echo cleared\r", NULL},
        {"printf 'BUSY_%s\\n' READ; read -r answer\r", "answer\r", "BUSY_READ"},
        {"printf 'BUSY_%s\\n' EDIT; read -e -r answer\r", "answer\r", "BUSY_EDIT"},
        {"printf 'BUSY_%s\\n' LOOP; while :; do :; done\r", "\003", "BUSY_LOOP"},
        {"printf 'BUSY_%s\\n' PASTE\rpartial", "\025echo recovered\r", "BUSY_PASTE"},
        {"echo 'unfinished\r", "completed'\r", NULL},
        {"sleep .6 & printf 'BUSY_%s\\n' BACKGROUND\r", NULL, "BUSY_BACKGROUND"}
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        c1_terminal_session session;
        c1_terminal_init(&session);
        expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "start recoverable busy-state fixture");
        expect(wait_output(&session, geteuid() == 0 ? "# " : "$ "), "recoverable fixture reached prompt");
        expect(retry_terminal_write(&session, cases[i].busy) == C1_STATUS_OK, "enter real busy state");
        if (cases[i].ready != NULL) expect(wait_output(&session, cases[i].ready), "busy fixture executed");
        expect_protected(&session, "incomplete input or actual work stays protected");
        if (cases[i].finish != NULL)
            expect(retry_terminal_write(&session, cases[i].finish) == C1_STATUS_OK, "complete or cancel current input/work");
        expect(wait_idle_close(&session), "protection is current-state only and recovers without restarting shell");
        c1_terminal_stop(&session);
    }
}

static void test_stale_prompt_and_macro(void)
{
    c1_terminal_session session;
    c1_terminal_init(&session);
    expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "start unread-prompt race fixture");
    expect(retry_terminal_write(&session,
        "echo old-prompt\rprintf 'STALE_%s\\n' BUSY; read -r answer\r") == C1_STATUS_OK,
        "queue another real task before observing any prompt");
    expect(wait_output(&session, "STALE_BUSY"), "read builtin started after an older empty prompt");
    expect_protected(&session, "old prompt bytes cannot authorize closing a newer builtin read");
    expect(retry_terminal_write(&session, "answer\r") == C1_STATUS_OK, "complete raced read");
    expect(wait_idle_close(&session), "fresh native proof recovers after raced read completes");
    c1_terminal_stop(&session);

    expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "start Readline macro fixture");
    expect(retry_terminal_write(&session,
        "bind '\"\\C-xq\":\"echo macro\\C-mpartial\"'; printf 'MACRO_%s\\n' READY\r") == C1_STATUS_OK,
        "install macro containing a complete command and an unfinished line");
    expect(wait_output(&session, "MACRO_READY"), "macro binding installed");
    expect(retry_terminal_write(&session, "\030q") == C1_STATUS_OK, "invoke Readline's private macro queue");
    expect_protected(&session, "private macro input and its unfinished result stay protected");
    expect(retry_terminal_write(&session, "\025echo macro-cleared\r") == C1_STATUS_OK,
           "clear only unfinished macro text through normal Readline editing");
    expect(wait_idle_close(&session), "completed macro editing does not permanently veto idleness");
    c1_terminal_stop(&session);
}

static void test_idle_unknown_and_queue(void)
{
    c1_terminal_session session;
    bool was_running = false;
    int control;
    int64_t deadline;
    c1_terminal_init(&session);
    expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "start unknown-state fixture");
    expect(wait_output(&session, geteuid() == 0 ? "# " : "$ "), "wait for unknown-state fixture prompt");
    session.pending[0] = 'x';
    session.pending_length = 1U;
    expect(!c1_terminal_close_idle(&session), "queued, not yet written input protects shell");
    session.pending_length = 0U;
    control = session.control_fd;
    session.control_fd = -1;
    expect(!c1_terminal_close_idle(&session), "missing supervisor channel protects shell");
    session.control_fd = control;
    session.state = C1_TERMINAL_FAILED;
    expect(!c1_terminal_close_idle(&session), "unknown lifecycle state protects shell");
    session.state = C1_TERMINAL_RUNNING;
    expect(c1_terminal_suspend(&session, &was_running) == C1_STATUS_OK && was_running,
           "explicit suspend retains its existing behavior");
    expect_protected(&session, "explicitly suspended shell remains protected");
    expect(c1_terminal_resume(&session, was_running) == C1_STATUS_OK, "explicit resume still works");
    expect(kill(session.shell_pid, SIGSTOP) == 0, "externally stop idle shell");
    deadline = now_ms() + 1000;
    while (process_state(session.shell_pid) != 'T' && now_ms() < deadline) pause_tick();
    expect_protected(&session, "kernel stop without UI suspended flag remains protected");
    expect(process_state(session.shell_pid) == 'T', "denial does not resume an externally stopped shell");
    expect(kill(session.shell_pid, SIGCONT) == 0, "restore externally stopped fixture");
    expect(wait_idle_close(&session), "restored untouched shell is still eligible");
    c1_terminal_stop(&session);
    {
        char *arguments[] = {"/bin/bash", "--noprofile", "--norc", "-i", NULL};
        expect(c1_terminal_start_exec(&session, 49, 19, arguments[0], arguments) == C1_STATUS_OK,
               "start direct-exec Bash lookalike");
        expect_protected(&session, "even a prompt-shaped direct-exec application is protected");
        c1_terminal_stop(&session);
    }
}

static void test_idle_request_input_race(void)
{
    c1_terminal_session session;
    c1_status result;
    c1_terminal_init(&session);
    expect(c1_terminal_start(&session, 49, 19) == C1_STATUS_OK, "start close/input race fixture");
    expect(wait_output(&session, geteuid() == 0 ? "# " : "$ "), "wait for race fixture prompt");
    expect(!c1_terminal_close_idle(&session), "first idle request waits for asynchronous reap");
    result = c1_terminal_write(&session, "read x\r", 7U);
    if (result == C1_STATUS_OK) {
        /* A conservative /proc denial may already have arrived. In that case
         * the new input wins and protects the active read until it completes. */
        expect(session.input_seen && !session.idle_close_pending,
               "new work is accepted only after an explicit close denial");
        expect_protected(&session, "input accepted after a denied request stays protected");
    } else {
        expect(result == C1_STATUS_UNAVAILABLE || result == C1_STATUS_INVALID_ARGUMENT,
               "in-flight idle close cannot accept new shell work");
        expect(!session.input_seen, "rejected racing input was never accepted into the PTY");
        expect(wait_idle_close(&session), "close/input race finishes without blocking");
    }
    c1_terminal_stop(&session);
}

static void test_idle_owner_crash(void)
{
    int report[2];
    pid_t process, identities[2] = {-1, -1};
    int64_t deadline;
    bool reaped = false;
    expect(pipe(report) == 0, "create idle-request owner-crash identity pipe");
    process = fork();
    if (process == 0) {
        c1_terminal_session session;
        close(report[0]);
        c1_terminal_init(&session);
        if (c1_terminal_start(&session, 49, 19) != C1_STATUS_OK) _exit(126);
        if (!wait_output(&session, geteuid() == 0 ? "# " : "$ ")) _exit(126);
        identities[0] = session.child_pid;
        identities[1] = session.shell_pid;
        if (write(report[1], identities, sizeof(identities)) != (ssize_t)sizeof(identities)) _exit(126);
        (void)c1_terminal_close_idle(&session);
        _exit(0); /* Includes the case where the helper is parking its proof. */
    }
    close(report[1]);
    expect(read(report[0], identities, sizeof(identities)) == (ssize_t)sizeof(identities),
           "record the owned idle shell and its supervisor");
    close(report[0]);
    if (process > 0) (void)waitpid(process, NULL, 0);
    deadline = now_ms() + 3000;
    while (identities[0] > 0 && now_ms() < deadline) {
        if (waitpid(identities[0], NULL, WNOHANG) == identities[0]) { reaped = true; break; }
        pause_tick();
    }
    expect(reaped, "crashed UI cannot leave a pending idle supervisor");
    expect(identities[1] > 0 && kill(identities[1], 0) < 0 && errno == ESRCH,
           "crashed UI cannot strand a shell in its native idle proof");
}

static void test_heartbeat(void)
{
    int pipefd[2];
    char value[32], byte;
    expect(!c1_liveness_expired(19999, 0, -1, 20000, 12000), "startup grace is bounded but tolerant");
    expect(c1_liveness_expired(20000, 0, -1, 20000, 12000), "startup hang detected before thirty-second confirmation");
    expect(!c1_liveness_expired(13000, 0, 2000, 20000, 12000), "normal slow operation has heartbeat grace");
    expect(c1_liveness_expired(14000, 0, 2000, 20000, 12000), "stopped UI pulses detect a hang");
    expect(pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0, "create anonymous heartbeat pipe");
    snprintf(value, sizeof(value), "%d", pipefd[1]);
    setenv(C1_HEARTBEAT_FD_ENV, value, 1);
    c1_liveness_init();
    c1_liveness_beat(1000);
    expect(read(pipefd[0], &byte, 1U) == 1 && byte == 'H', "loop sends an anonymous pipe heartbeat");
    c1_liveness_beat(1001);
    expect(read(pipefd[0], &byte, 1U) < 0 && errno == EAGAIN, "heartbeat rate is low even in busy loops");
    {
        pid_t child = fork();
        if (child == 0) { c1_liveness_beat(4000); _exit(0); }
        waitpid(child, NULL, 0);
        expect(read(pipefd[0], &byte, 1U) < 0 && errno == EAGAIN,
               "forked workers cannot fake UI loop progress");
    }
    expect((fcntl(pipefd[1], F_GETFD) & FD_CLOEXEC) != 0, "heartbeat cannot leak to exec applications");
    close(pipefd[0]);
    c1_liveness_beat(5000); /* Closed reader must not kill the UI with SIGPIPE. */
    c1_liveness_close();
}

int main(int argc, char **argv)
{
    char self[4096];
    ssize_t count;
    if (argc > 1) return fixture(argc, argv);
    count = readlink("/proc/self/exe", self, sizeof(self)-1U);
    if (count <= 0) return 1;
    self[count] = '\0';
    expect(c1_descendants_adopt() == 0, "test can reap the deliberately orphaned supervisor");
    test_cleanup(self, "--foreground");
    test_cleanup(self, "--shell-first");
    test_cleanup(self, "--detached");
    test_parent_crash(self);
    test_busy_terminal_app();
    test_idle_shell();
    test_idle_input_protection();
    test_commands_become_idle();
    test_busy_then_idle();
    test_stale_prompt_and_macro();
    test_idle_unknown_and_queue();
    test_idle_request_input_race();
    test_idle_owner_crash();
    test_heartbeat();
    if (failures) return 1;
    puts("all terminal lifecycle tests passed");
    return 0;
}
