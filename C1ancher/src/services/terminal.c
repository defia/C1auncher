#define _GNU_SOURCE 1
#define _XOPEN_SOURCE 600

#include "services/terminal.h"
#include "services/terminal_idle_protocol.h"
#include "platform/liveness.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifdef C1_TERMINAL_IDLE_BLOB_HEADER
#include C1_TERMINAL_IDLE_BLOB_HEADER

/* Created only in the shell child. No pathname survives startup, even if
 * Bash cannot load the module. Keep only a read-only fd across the first exec. */
static int embedded_idle_helper(void)
{
    char path[] = "/dev/shm/c1-terminal-idle-XXXXXX";
    char descriptor[64];
    int writer = mkstemp(path), reader = -1;
    size_t offset = 0U;
    if (writer < 0) return -1;
    if (fcntl(writer, F_SETFD, FD_CLOEXEC) != 0) goto done;
    while (offset < sizeof(c1_terminal_idle_image)) {
        ssize_t count = write(writer, c1_terminal_idle_image + offset,
                              sizeof(c1_terminal_idle_image) - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) goto done;
        offset += (size_t)count;
    }
    if (fchmod(writer, S_IRUSR) != 0) goto done;
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", writer);
    reader = open(descriptor, O_RDONLY | O_CLOEXEC);
    if (reader >= 0 && fcntl(reader, F_SETFD, 0) != 0) {
        close(reader);
        reader = -1;
    }
done:
    if (unlink(path) != 0 && reader >= 0) { close(reader); reader = -1; }
    close(writer);
    return reader;
}
#endif

#define C1_TERMINAL_STOP_GRACE_MS 500
#define C1_TERMINAL_IDLE_CHECK_MS 40
#define C1_TERMINAL_IDLE_QUERY_MS 60
#define C1_TERMINAL_IDLE_REQUEST 'I'
#define C1_TERMINAL_IDLE_DENIED 'N'
#define C1_TERMINAL_IDLE_ACCEPTED 'Y'

static int64_t monotonic_milliseconds(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return -1;
    }
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static void reset_signals(void)
{
    struct sigaction action;
    sigset_t mask;
    int signal_number;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (signal_number = 1; signal_number < NSIG; ++signal_number) {
        if (signal_number != SIGKILL && signal_number != SIGSTOP) {
            (void)sigaction(signal_number, &action, NULL);
        }
    }
    sigemptyset(&mask);
    (void)sigprocmask(SIG_SETMASK, &mask, NULL);
}

static void close_master(c1_terminal_session *session)
{
    if (session->master_fd >= 0) {
        close(session->master_fd);
        session->master_fd = -1;
    }
    if (session->control_fd >= 0) {
        close(session->control_fd);
        session->control_fd = -1;
    }
    session->pending_offset = 0U;
    session->pending_length = 0U;
    session->idle_close_pending = false;
    session->idle_close_accepted = false;
}

static void reap_child(c1_terminal_session *session, bool block)
{
    int status;
    pid_t result;

    if (session == NULL || session->child_pid <= 0) {
        return;
    }
    do {
        result = waitpid(session->child_pid, &status, block ? 0 : WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result != session->child_pid) {
        return;
    }
    session->child_pid = -1;
    close_master(session);
    if (WIFEXITED(status)) {
        session->exit_code = WEXITSTATUS(status);
        session->state = C1_TERMINAL_EXITED;
    } else if (WIFSIGNALED(status)) {
        session->exit_code = 128 + WTERMSIG(status);
        session->state = C1_TERMINAL_EXITED;
    } else {
        session->exit_code = 128;
        session->state = C1_TERMINAL_FAILED;
    }
}

void c1_terminal_init(c1_terminal_session *session)
{
    if (session == NULL) {
        return;
    }
    memset(session, 0, sizeof(*session));
    session->master_fd = -1;
    session->child_pid = -1;
    session->shell_pid = -1;
    session->control_fd = -1;
    session->state = C1_TERMINAL_STOPPED;
}

static int open_pty_master(char *slave_name, size_t capacity)
{
    int master;
    char *name;

    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        if (master >= 0) {
            close(master);
        }
        return -1;
    }
    name = ptsname(master);
    if (name == NULL || strlen(name) + 1U > capacity) {
        close(master);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(slave_name, name, strlen(name) + 1U);
    return master;
}

static void child_exec(int master,
                       const char *slave_name,
                       unsigned int columns,
                       unsigned int rows,
                       const char *path, char *const argv[], int prompt)
{
    int slave;
#ifdef C1_TERMINAL_IDLE_BLOB_HEADER
    int image_fd = -1;
#endif
    struct winsize window;

    reset_signals();
    if (prctl(PR_SET_PDEATHSIG, SIGHUP) != 0 || getppid() == 1 || setsid() < 0) {
        _exit(126);
    }
    slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0 || ioctl(slave, TIOCSCTTY, 0) != 0) {
        _exit(126);
    }
    memset(&window, 0, sizeof(window));
    window.ws_col = (unsigned short)columns;
    window.ws_row = (unsigned short)rows;
    if (ioctl(slave, TIOCSWINSZ, &window) != 0 ||
        dup2(slave, STDIN_FILENO) < 0 ||
        dup2(slave, STDOUT_FILENO) < 0 ||
        dup2(slave, STDERR_FILENO) < 0) {
        _exit(126);
    }
    if (slave > STDERR_FILENO) {
        close(slave);
    }
    close(master);
    (void)setenv("TERM", "xterm-256color", 1);
    {
        char terminal_columns[16], terminal_rows[16];
        snprintf(terminal_columns, sizeof(terminal_columns), "%u", columns);
        snprintf(terminal_rows, sizeof(terminal_rows), "%u", rows);
        (void)setenv("COLUMNS", terminal_columns, 1);
        (void)setenv("LINES", terminal_rows, 1);
    }
    (void)setenv("PATH", "/usr/data/c1/bin:/sbin:/usr/sbin:/bin:/usr/bin", 1);
    (void)setenv("SHELL", "/bin/bash", 1);
    (void)setenv("HOME", "/root", 1);
    (void)setenv("USER", "root", 1);
    (void)setenv("LOGNAME", "root", 1);
    (void)setenv("C1_C1ANCHER_TERMINAL", "1", 1);
    if (access("/usr/data", X_OK) == 0 && chdir("/usr/data") != 0 && chdir("/") != 0) {
        _exit(126);
    }
    if (path != NULL) {
        close(prompt);
        execv(path, argv);
        _exit(127);
    }
    {
        char descriptor[32];
        extern char **environ;
        /* A prompt notification is trustworthy only with our own startup
         * configuration. Inherited Bash functions/options could otherwise
         * execute arbitrary work after PROMPT_COMMAND and before Readline. */
        for (size_t i = 0U; environ[i] != NULL;) {
            if (strncmp(environ[i], "BASH_FUNC_", 10U) == 0) {
                char *name = strdup(environ[i]);
                char *equal = name != NULL ? strchr(name, '=') : NULL;
                if (equal == NULL) _exit(126);
                *equal = '\0';
                if (unsetenv(name) != 0) _exit(126);
                free(name);
            } else ++i;
        }
        if (unsetenv("SHELLOPTS") != 0 || unsetenv("BASHOPTS") != 0 ||
            unsetenv("BASH_ENV") != 0 || unsetenv("ENV") != 0 ||
            setenv("PS0", "", 1) != 0 || setenv("PS1", "\\s-\\v\\$ ", 1) != 0 ||
            setenv("PS2", "> ", 1) != 0 || setenv("PS4", "+ ", 1) != 0 ||
            /* Missing private symbols must fail dlopen, not kill Bash later
             * when a lazy PLT entry is first called from the builtin. */
            setenv("LD_BIND_NOW", "1", 1) != 0 ||
            fcntl(prompt, F_SETFD, 0) != 0) _exit(126);
        snprintf(descriptor, sizeof(descriptor), "%d", prompt);
        if (setenv(C1_TERMINAL_IDLE_FD_ENV, descriptor, 1) != 0) _exit(126);
#ifdef C1_TERMINAL_IDLE_BLOB_HEADER
        {
            char helper[64], startup[1024];
            image_fd = embedded_idle_helper();
            /* Production never trusts an inherited helper path. */
            (void)unsetenv(C1_TERMINAL_IDLE_HELPER_ENV);
            (void)unsetenv(C1_TERMINAL_IDLE_IMAGE_FD_ENV);
            if (image_fd < 0) {
                close(prompt);
                (void)setenv("PROMPT_COMMAND", ":", 1);
                fputs("C1ancher: terminal idle verification unavailable (embedded module); "
                      "automatic shutdown is protected.\n", stderr);
            } else {
                snprintf(helper, sizeof(helper), "/proc/self/fd/%d", image_fd);
                snprintf(descriptor, sizeof(descriptor), "%d", image_fd);
                if (setenv(C1_TERMINAL_IDLE_HELPER_ENV, helper, 1) != 0 ||
                    setenv(C1_TERMINAL_IDLE_IMAGE_FD_ENV, descriptor, 1) != 0) _exit(126);
                /* Close the image even when dlopen/ABI installation fails. On
                 * success the helper closes it and unsets IMAGE_FD itself. */
                snprintf(startup, sizeof(startup),
                    "{ if [[ ${C1_TERMINAL_IDLE_IMAGE_FD+x} ]]; then "
                    "if builtin enable -f \"$C1_TERMINAL_IDLE_HELPER\" c1_terminal_idle && "
                    "builtin c1_terminal_idle; then :; else "
                    "builtin echo 'C1ancher: terminal idle module incompatible; automatic shutdown is protected.' >&2; "
                    "builtin eval 'exec %d<&-'; fi; "
                    "if [[ ${C1_TERMINAL_IDLE_IMAGE_FD+x} ]]; then builtin eval 'exec %d<&-'; fi; "
                    "builtin unset C1_TERMINAL_IDLE_IMAGE_FD C1_TERMINAL_IDLE_HELPER; "
                    "else builtin c1_terminal_idle 2>/dev/null; fi; }", prompt, image_fd);
                if (setenv("PROMPT_COMMAND", startup, 1) != 0) _exit(126);
            }
        }
#else
        /* Standalone builds must opt in explicitly; never infer a sidecar. */
        if (setenv("PROMPT_COMMAND",
                   "{ builtin type -t c1_terminal_idle >/dev/null || "
                   "builtin enable -f \"$C1_TERMINAL_IDLE_HELPER\" c1_terminal_idle; "
                   "builtin c1_terminal_idle; } 2>/dev/null", 1) != 0) _exit(126);
#endif
    }
    execl("/bin/bash", "bash", "--noprofile", "--norc", "-i", (char *)NULL);
    /* No prompt proof is available for the fallback shell. */
#ifdef C1_TERMINAL_IDLE_BLOB_HEADER
    if (image_fd >= 0) close(image_fd);
#endif
    close(prompt);
    (void)unsetenv("PROMPT_COMMAND");
    (void)setenv("SHELL", "/bin/sh", 1);
    execl("/bin/sh", "sh", "-i", (char *)NULL);
    _exit(127);
}

/* With the shell stopped and unreaped, each descendant tree has a stable
 * anchor: a direct shell child (even a zombie), or a child already adopted by
 * this single-threaded subreaper. Neither anchor can disappear during this
 * scan because neither parent reaps here. Session/process-group membership is
 * intentionally irrelevant, so disowned, stopped and setsid jobs stay safe.
 * Do not use /proc/PID/task/TID/children: the device kernel omits it. */
static bool idle_tree_empty(pid_t shell, int64_t deadline)
{
    DIR *directory = opendir("/proc");
    struct dirent *entry;
    bool okay = false, saw_shell = false;
    size_t entries = 0U;
    if (directory == NULL) return false;
    for (;;) {
        char *end, path[64], data[4096], state;
        long pid, parent;
        FILE *file;
        size_t count;
        int64_t now = monotonic_milliseconds();
        if (now < 0 || now >= deadline || ++entries > 16384U) break;
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            okay = errno == 0 && saw_shell;
            break;
        }
        if (entry->d_name[0] < '1' || entry->d_name[0] > '9') continue;
        errno = 0;
        pid = strtol(entry->d_name, &end, 10);
        if (errno != 0 || *end != '\0' || pid <= 0 || pid > INT_MAX) break;
        snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        file = fopen(path, "r");
        /* Even a disappearing unrelated process conservatively denies this
         * attempt. A later deadline check may retry with a clean snapshot. */
        if (file == NULL) break;
        count = fread(data, 1U, sizeof(data) - 1U, file);
        if (ferror(file) || count == 0U || count == sizeof(data) - 1U) {
            fclose(file);
            break;
        }
        if (fclose(file) != 0) break;
        data[count] = '\0';
        end = strrchr(data, ')');
        if (end == NULL || sscanf(end + 1, " %c %ld", &state, &parent) != 2 || parent < 0)
            break;
        if (pid == shell) {
            if (parent != (long)getpid() || state != 'T') break;
            saw_shell = true;
        } else if (parent == (long)shell || parent == (long)getpid()) break;
    }
    if (closedir(directory) != 0) okay = false;
    return okay;
}

static bool supervisor_close_idle(int master, pid_t shell, bool prompt_seen)
{
    siginfo_t information;
    int64_t now = monotonic_milliseconds();
    int64_t deadline;
    bool frozen = false, accepted = false;
    if (!prompt_seen || now < 0 || tcgetpgrp(master) != shell) return false;
    deadline = now + C1_TERMINAL_IDLE_CHECK_MS;
    memset(&information, 0, sizeof(information));
    /* WNOWAIT proves actual parenthood without releasing the PID. An already
     * stopped shell is protected; only our own SIGSTOP is ever undone. */
    if (waitid(P_PID, (id_t)shell, &information,
               WEXITED | WSTOPPED | WNOHANG | WNOWAIT) != 0 || information.si_pid != 0)
        return false;
    if (kill(shell, SIGSTOP) != 0) return false;
    for (;;) {
        struct timespec delay = {0, 1000000L};
        memset(&information, 0, sizeof(information));
        if (waitid(P_PID, (id_t)shell, &information,
                   WEXITED | WSTOPPED | WNOHANG | WNOWAIT) != 0) break;
        if (information.si_pid == shell) {
            frozen = information.si_code == CLD_STOPPED && information.si_status == SIGSTOP;
            break;
        }
        now = monotonic_milliseconds();
        if (now < 0 || now >= deadline) break;
        (void)nanosleep(&delay, NULL);
    }
    if (frozen && tcgetpgrp(master) == shell && idle_tree_empty(shell, deadline)) {
        /* Never call descendant cleanup to decide idleness. Only this proven
         * unreaped direct child is signalled after its entire tree is empty. */
        accepted = kill(shell, SIGKILL) == 0;
    }
    if (!accepted) (void)kill(shell, SIGCONT);
    return accepted;
}

static bool request_shell_idle(int channel, int master, pid_t shell, uint64_t token)
{
    c1_terminal_idle_message query = {C1_TERMINAL_IDLE_MAGIC, C1_TERMINAL_IDLE_QUERY, token, 0};
    int64_t now = monotonic_milliseconds();
    bool accepted = false;
    if (now < 0) return false;
    query.deadline_ms = now + C1_TERMINAL_IDLE_QUERY_MS;
    if (send(channel, &query, sizeof(query), MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)sizeof(query))
        return false;
    for (unsigned int attempts = 0U; attempts < 32U; ++attempts) {
        c1_terminal_idle_message proof;
        struct pollfd descriptor = {channel, POLLIN, 0};
        struct iovec vector = {&proof, sizeof(proof)};
        union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(struct ucred))]; } credentials;
        struct msghdr message;
        struct cmsghdr *header;
        bool owned = false;
        ssize_t count;
        int event;
        now = monotonic_milliseconds();
        if (now < 0 || now >= query.deadline_ms) break;
        event = poll(&descriptor, 1U, (int)(query.deadline_ms - now));
        if (event < 0 && errno == EINTR) continue;
        if (event <= 0) break;
        memset(&message, 0, sizeof(message));
        message.msg_iov = &vector;
        message.msg_iovlen = 1U;
        message.msg_control = credentials.bytes;
        message.msg_controllen = sizeof(credentials.bytes);
        count = recvmsg(channel, &message, MSG_DONTWAIT | MSG_TRUNC);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count <= 0) break;
        if (count != (ssize_t)sizeof(proof) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0)
            continue;
        for (header = CMSG_FIRSTHDR(&message); header != NULL; header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_CREDENTIALS &&
                header->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {
                struct ucred sender;
                memcpy(&sender, CMSG_DATA(header), sizeof(sender));
                owned = sender.pid == shell;
            }
        }
        if (!owned || proof.magic != C1_TERMINAL_IDLE_MAGIC || proof.token != token) continue;
        if (proof.kind == C1_TERMINAL_IDLE_PROOF)
            accepted = supervisor_close_idle(master, shell, true);
        break;
    }
    /* Release even a timed-out query: the hook may have answered just as our
     * timer expired. It is never left parked after a failed check. A broken
     * channel is shut down so its reader wakes on EOF instead. */
    query.kind = C1_TERMINAL_IDLE_RELEASE;
    if (send(channel, &query, sizeof(query), MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)sizeof(query))
        (void)shutdown(channel, SHUT_RDWR);
    return accepted;
}

static void session_child_changed(int signal_number)
{
    (void)signal_number;
}

static void supervise_terminal(int master, const char *slave_name,
                               unsigned int columns, unsigned int rows,
                               const char *path, char *const argv[],
                               int control, int report)
{
    pid_t shell;
    int status = 0;
    bool exited = false;
    uint64_t idle_token = 0U;
    int prompt[2], credentials = 1;
    sigset_t blocked, original;
    struct sigaction action;
    reset_signals();
    memset(&action, 0, sizeof(action));
    action.sa_handler = session_child_changed;
    action.sa_flags = SA_NOCLDSTOP;
    sigemptyset(&action.sa_mask);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigaction(SIGCHLD, &action, NULL) != 0 ||
        sigprocmask(SIG_BLOCK, &blocked, &original) != 0) _exit(126);
    c1_liveness_close();
    {
        DIR *fds = opendir("/proc/self/fd");
        struct dirent *entry;
        if (fds == NULL) _exit(126);
        while ((entry = readdir(fds)) != NULL) {
            char *end;
            long fd = strtol(entry->d_name, &end, 10);
            if (*end == '\0' && fd > STDERR_FILENO && fd != master &&
                fd != control && fd != report && fd != dirfd(fds))
                (void)close((int)fd);
        }
        (void)closedir(fds);
    }
    if (c1_descendants_adopt() != 0 ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, prompt) != 0 ||
        setsockopt(prompt[0], SOL_SOCKET, SO_PASSCRED, &credentials, sizeof(credentials)) != 0)
        _exit(126);
    shell = fork();
    if (shell < 0) _exit(126);
    if (shell == 0) {
        close(control);
        close(report);
        close(prompt[0]);
        child_exec(master, slave_name, columns, rows, path, argv, prompt[1]);
    }
    close(prompt[1]);
    if (write(report, &shell, sizeof(shell)) != (ssize_t)sizeof(shell)) {
        (void)c1_descendants_cleanup(C1_TERMINAL_STOP_GRACE_MS);
        _exit(126);
    }
    close(report);
    for (;;) {
        struct pollfd command = {control, POLLIN, 0};
        pid_t result = waitpid(shell, &status, WNOHANG);
        if (result == shell) { exited = true; break; }
        if (result < 0 && errno != EINTR) break;
        {
            int event = ppoll(&command, 1U, NULL, &original);
            if (event < 0 && errno != EINTR) break;
            if (event > 0) {
                char request[2], response = C1_TERMINAL_IDLE_DENIED;
                ssize_t count = recv(control, request, sizeof(request), MSG_DONTWAIT);
                if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR)) break;
                if (count < 0) continue;
                if (count == 1 && request[0] == C1_TERMINAL_IDLE_REQUEST && path == NULL &&
                    ++idle_token != 0U && request_shell_idle(prompt[0], master, shell, idle_token))
                    response = C1_TERMINAL_IDLE_ACCEPTED;
                if (send(control, &response, 1U, MSG_DONTWAIT | MSG_NOSIGNAL) != 1) break;
            }
        }
    }
    close(prompt[0]);
    /* Cleanup does not depend on shell survival or a foreground group leader.
     * The subreaper owns every PID it signals, including detached grandchildren. */
    (void)c1_descendants_cleanup(C1_TERMINAL_STOP_GRACE_MS);
    close(master);
    close(control);
    _exit(exited && WIFEXITED(status) ? WEXITSTATUS(status) :
          exited && WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 0);
}

static c1_status start_session(c1_terminal_session *session,
                               unsigned int columns, unsigned int rows,
                               const char *path, char *const argv[])
{
    char slave_name[64];
    int master;
    pid_t child;
    int flags;
    int control[2], report[2];

    if (session == NULL || columns == 0U || rows == 0U || columns > UINT16_MAX ||
        rows > UINT16_MAX) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    if (c1_terminal_is_running(session)) {
        return C1_STATUS_OK;
    }
    c1_terminal_stop(session);
    if (session->child_pid > 0) return C1_STATUS_UNAVAILABLE;
    master = open_pty_master(slave_name, sizeof(slave_name));
    if (master < 0) {
        session->state = C1_TERMINAL_FAILED;
        return C1_STATUS_IO_ERROR;
    }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, control) != 0) {
        close(master);
        return C1_STATUS_IO_ERROR;
    }
    if (pipe2(report, O_CLOEXEC) != 0) {
        close(master); close(control[0]); close(control[1]);
        return C1_STATUS_IO_ERROR;
    }
    child = fork();
    if (child < 0) {
        close(master); close(control[0]); close(control[1]);
        close(report[0]); close(report[1]);
        session->state = C1_TERMINAL_FAILED;
        return C1_STATUS_IO_ERROR;
    }
    if (child == 0) {
        close(control[1]); close(report[0]);
        supervise_terminal(master, slave_name, columns, rows, path, argv,
                           control[0], report[1]);
    }
    close(control[0]); close(report[1]);
    {
        ssize_t count;
        do { count = read(report[0], &session->shell_pid, sizeof(session->shell_pid)); }
        while (count < 0 && errno == EINTR);
        close(report[0]);
        if (count != (ssize_t)sizeof(session->shell_pid)) {
            close(master); close(control[1]);
            (void)kill(child, SIGKILL);
            while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
            return C1_STATUS_IO_ERROR;
        }
    }
    session->control_fd = control[1];
    session->direct_exec = path != NULL;
    flags = fcntl(master, F_GETFL, 0);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(master);
        close(session->control_fd);
        session->control_fd = -1;
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
        }
        session->state = C1_TERMINAL_FAILED;
        return C1_STATUS_IO_ERROR;
    }
    session->master_fd = master;
    session->child_pid = child;
    session->state = C1_TERMINAL_RUNNING;
    session->exit_code = 0;
    session->pending_offset = 0U;
    session->pending_length = 0U;
    session->suspended = false;
    session->input_seen = false;
    session->idle_close_pending = false;
    session->idle_close_accepted = false;
    return C1_STATUS_OK;
}

c1_status c1_terminal_start(c1_terminal_session *session,
                            unsigned int columns, unsigned int rows)
{
    return start_session(session, columns, rows, NULL, NULL);
}

c1_status c1_terminal_start_exec(c1_terminal_session *session,
                                 unsigned int columns, unsigned int rows,
                                 const char *path, char *const argv[])
{
    if (path == NULL || path[0] != '/' || argv == NULL || argv[0] == NULL)
        return C1_STATUS_INVALID_ARGUMENT;
    if (session == NULL || c1_terminal_is_running(session))
        return C1_STATUS_INVALID_ARGUMENT;
    return start_session(session, columns, rows, path, argv);
}

int c1_terminal_fd(const c1_terminal_session *session)
{
    return session != NULL && session->state == C1_TERMINAL_RUNNING ? session->master_fd : -1;
}

short c1_terminal_poll_events(const c1_terminal_session *session)
{
    short events = POLLIN;

    if (session != NULL && session->pending_length > 0U) {
        events |= POLLOUT;
    }
    return events;
}

bool c1_terminal_is_running(c1_terminal_session *session)
{
    if (session == NULL) {
        return false;
    }
    reap_child(session, false);
    return session->state == C1_TERMINAL_RUNNING && session->master_fd >= 0;
}

bool c1_terminal_shell_is_foreground(c1_terminal_session *session)
{
    pid_t foreground;

    if (!c1_terminal_is_running(session) || session->child_pid <= 0) {
        return false;
    }
    foreground = tcgetpgrp(session->master_fd);
    return !session->direct_exec && foreground > 0 && foreground == session->shell_pid;
}

bool c1_terminal_command_running(c1_terminal_session *session)
{
    pid_t foreground;

    /* Automatic shutdown only waits for a command that currently owns the
     * terminal foreground process group. Typed-but-unsubmitted input,
     * background/stopped/detached jobs and unknown terminal state are not
     * shutdown blockers; the session cleanup below will terminate them. */
    if (session == NULL || session->direct_exec || !c1_terminal_is_running(session) ||
        session->shell_pid <= 0 || session->master_fd < 0) {
        return false;
    }
    foreground = tcgetpgrp(session->master_fd);
    return foreground > 0 && foreground != session->shell_pid;
}

static void idle_close_reply(c1_terminal_session *session)
{
    char response;
    ssize_t count;
    if (!session->idle_close_pending || session->idle_close_accepted) return;
    count = recv(session->control_fd, &response, 1U, MSG_DONTWAIT);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    if (count == 1 && response == C1_TERMINAL_IDLE_ACCEPTED) {
        session->idle_close_accepted = true;
        return;
    }
    session->idle_close_pending = false;
    if (count != 1 || response != C1_TERMINAL_IDLE_DENIED) session->input_seen = true;
}

bool c1_terminal_close_idle(c1_terminal_session *session)
{
    char request = C1_TERMINAL_IDLE_REQUEST;
    if (session == NULL) return false;
    reap_child(session, false);
    if (session->child_pid <= 0)
        return session->master_fd < 0 && session->control_fd < 0 &&
               (session->state == C1_TERMINAL_STOPPED || session->state == C1_TERMINAL_EXITED);
    if (session->state != C1_TERMINAL_RUNNING || session->master_fd < 0 ||
        session->shell_pid <= 0 || session->control_fd < 0 || session->direct_exec ||
        session->suspended || session->pending_length != 0U) return false;
    if (session->idle_close_pending) {
        /* A denial finishes this attempt. Do not immediately requeue: callers
         * waiting briefly for the reply must be able to observe pending=false. */
        idle_close_reply(session);
        return false;
    }
    if (send(session->control_fd, &request, 1U, MSG_DONTWAIT | MSG_NOSIGNAL) == 1)
        session->idle_close_pending = true;
    /* No waits, /proc scans or destructive stop in the UI process. The actual
     * parent freezes/checks its shell and replies, then exits/reaps normally. */
    return false;
}

c1_terminal_state c1_terminal_get_state(c1_terminal_session *session)
{
    if (session == NULL) {
        return C1_TERMINAL_FAILED;
    }
    (void)c1_terminal_is_running(session);
    return session->state;
}

int c1_terminal_exit_code(const c1_terminal_session *session)
{
    return session != NULL ? session->exit_code : 128;
}

c1_status c1_terminal_resize(c1_terminal_session *session, unsigned int columns, unsigned int rows)
{
    if (!session || columns == 0 || rows == 0 || columns > 1000 || rows > 1000)
        return C1_STATUS_INVALID_ARGUMENT;
    if (session->master_fd < 0) return C1_STATUS_OK;
    struct winsize window = {0};
    window.ws_col = (unsigned short)columns;
    window.ws_row = (unsigned short)rows;
    return ioctl(session->master_fd, TIOCSWINSZ, &window) == 0 ? C1_STATUS_OK : C1_STATUS_IO_ERROR;
}

c1_status c1_terminal_flush(c1_terminal_session *session)
{
    if (session != NULL && session->pending_length > 0U) {
        idle_close_reply(session);
        if (session->idle_close_pending) return C1_STATUS_UNAVAILABLE;
        session->input_seen = true;
    }
    while (session != NULL && session->pending_length > 0U) {
        ssize_t written = write(session->master_fd,
                                session->pending + session->pending_offset,
                                session->pending_length);

        if (written > 0) {
            session->pending_offset += (size_t)written;
            session->pending_length -= (size_t)written;
            if (session->pending_length == 0U) {
                session->pending_offset = 0U;
            }
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return C1_STATUS_OK;
        }
        if (written < 0 && errno == EIO) {
            reap_child(session, false);
            if (session->state == C1_TERMINAL_RUNNING && session->child_pid > 0) {
                return C1_STATUS_OK;
            }
        } else {
            reap_child(session, false);
        }
        return C1_STATUS_IO_ERROR;
    }
    return C1_STATUS_OK;
}

c1_status c1_terminal_write(c1_terminal_session *session, const void *bytes, size_t count)
{
    const char *source = bytes;

    if (session == NULL || bytes == NULL || count == 0U || !c1_terminal_is_running(session)) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    idle_close_reply(session);
    if (session->idle_close_pending) return C1_STATUS_UNAVAILABLE;
    if (session->pending_offset > 0U &&
        session->pending_offset + session->pending_length + count > sizeof(session->pending)) {
        memmove(session->pending,
                session->pending + session->pending_offset,
                session->pending_length);
        session->pending_offset = 0U;
    }
    if (count > sizeof(session->pending) - session->pending_offset - session->pending_length) {
        return C1_STATUS_UNAVAILABLE;
    }
    /* Retained for callers' diagnostics only. Current Readline state, not
     * historical input, is the supervisor's source of idle proof. */
    session->input_seen = true;
    memcpy(session->pending + session->pending_offset + session->pending_length, source, count);
    session->pending_length += count;
    return c1_terminal_flush(session);
}

ssize_t c1_terminal_read(c1_terminal_session *session, void *buffer, size_t capacity)
{
    ssize_t count;

    if (session == NULL || buffer == NULL || capacity == 0U || session->master_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    do {
        count = read(session->master_fd, buffer, capacity);
    } while (count < 0 && errno == EINTR);
    if (count > 0) {
        return count;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if (count == 0 || (count < 0 && errno == EIO)) {
        reap_child(session, false);
        return 0;
    }
    return -1;
}

c1_status c1_terminal_suspend(c1_terminal_session *session, bool *was_running)
{
    pid_t foreground;

    if (session == NULL || was_running == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    idle_close_reply(session);
    if (session->idle_close_pending) return C1_STATUS_UNAVAILABLE;
    *was_running = c1_terminal_is_running(session);
    if (!*was_running) {
        return C1_STATUS_OK;
    }
    foreground = tcgetpgrp(session->master_fd);
    if (foreground <= 0 || kill(-foreground, SIGSTOP) != 0) {
        return C1_STATUS_IO_ERROR;
    }
    session->suspended = true;
    return C1_STATUS_OK;
}

c1_status c1_terminal_resume(c1_terminal_session *session, bool was_running)
{
    pid_t foreground;

    if (session == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    if (!was_running || !session->suspended) {
        return C1_STATUS_OK;
    }
    foreground = session->master_fd >= 0 ? tcgetpgrp(session->master_fd) : -1;
    if (foreground <= 0 || kill(-foreground, SIGCONT) != 0) {
        c1_terminal_stop(session);
        return C1_STATUS_IO_ERROR;
    }
    session->suspended = false;
    return C1_STATUS_OK;
}

void c1_terminal_stop(c1_terminal_session *session)
{
    int64_t now, deadline;
    if (session == NULL) return;
    /* Closing the control channel asks the per-session subreaper to clean every
     * descendant, even when the interactive shell already exited. */
    close_master(session);
    now = monotonic_milliseconds();
    deadline = now < 0 ? -1 : now + C1_TERMINAL_STOP_GRACE_MS + 1000;
    while (session->child_pid > 0) {
        struct timespec pause = {0, 10000000L};
        reap_child(session, false);
        if (session->child_pid <= 0) break;
        now = monotonic_milliseconds();
        if (deadline < 0 || now < 0 || now >= deadline) {
            /* This is our unreaped child: the PID cannot have been reused. */
            (void)kill(session->child_pid, SIGKILL);
            reap_child(session, false);
            break;
        }
        (void)nanosleep(&pause, NULL);
    }
    session->state = C1_TERMINAL_STOPPED;
    session->exit_code = 0;
    session->suspended = false;
}
