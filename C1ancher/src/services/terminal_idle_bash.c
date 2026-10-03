/* Bash loadable builtin; build separately, NEVER link into the desktop:
 * cc -std=c11 -O2 -fPIC -shared -nostdlib -Wall -Wextra -Wpedantic -Werror
 *    src/services/terminal_idle_bash.c -o build/c1-terminal-idle.so
 * Embedded into C1ancher by Makefile; never installed as a fifth component.
 * -nostdlib avoids introducing a build-host libc
 * version dependency; libc and Bash/Readline symbols resolve from the running
 * dynamically linked Bash. Missing symbols/version support fails closed.
 * The small declarations below are the Bash loadable-builtin ABI and Readline
 * public state ABI, validated for Bash 5.1/Readline 8.1 and Bash 5.2/Readline
 * 8.2. Unknown Readline versions are rejected before accessing state.
 * A target Bash must independently export the private symbols below.
 */
#define _GNU_SOURCE 1
#include "terminal_idle_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct word_list;
struct builtin {
    char *name;
    int (*function)(struct word_list *);
    int flags;
    char *const *long_doc;
    const char *short_doc;
    char *handle;
};
extern int (*rl_event_hook)(void);
extern unsigned long rl_readline_state; /* native long: MIPS o32=4, host LP64=8 */
extern int rl_end, rl_point, rl_done, rl_pending_input, rl_readline_version;
extern char *rl_line_buffer, *rl_executing_macro;
extern FILE *rl_instream;
extern int executing, executing_builtin, parse_and_execute_level;
extern int get_current_prompt_level(void);
extern int _rl_pushed_input_available(void);
extern int rl_set_keyboard_input_timeout(int);

static int channel = -1;
static pid_t owner;
static int (*previous_hook)(void);

static int64_t idle_now(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static bool readline_idle(void)
{
    /* Only passive flags are allowed. In particular, reject MOREINPUT,
     * DISPATCHING, MULTIKEY, all search/completion/macro states and future
     * unknown flags. Readline's private pushback is checked independently. */
    const unsigned long passive = 0x2UL | 0x4UL | 0x8UL | 0x2000UL |
                                  0x40000UL | 0x80000UL | 0x400000UL;
    struct termios attributes;
    struct pollfd input;
    int queued = -1;
    if (getpid() != owner || executing || executing_builtin || parse_and_execute_level ||
        get_current_prompt_level() != 1 || rl_done || rl_end != 0 || rl_point != 0 ||
        rl_line_buffer == NULL || rl_line_buffer[0] != '\0' || rl_pending_input ||
        rl_executing_macro != NULL || _rl_pushed_input_available() ||
        (rl_readline_state & ~passive) != 0UL || (rl_readline_state & 0x6UL) != 0x6UL ||
        rl_instream == NULL) return false;
    input.fd = fileno(rl_instream);
    input.events = POLLIN;
    input.revents = 0;
    /* With canonical mode enabled FIONREAD can hide incomplete input. The
     * native Readline path must be noncanonical; unknown modes are rejected. */
    return input.fd == STDIN_FILENO && tcgetattr(input.fd, &attributes) == 0 &&
           (attributes.c_lflag & ICANON) == 0 && tcgetpgrp(input.fd) == owner &&
           ioctl(input.fd, FIONREAD, &queued) == 0 && queued == 0 &&
           poll(&input, 1U, 0) == 0;
}

static void park_proof(uint64_t token)
{
    /* No independent timeout may revoke a proof while the parent freezes us.
     * Every parent outcome releases the token; parent death produces EOF. */
    for (;;) {
        c1_terminal_idle_message release;
        struct pollfd control = {channel, POLLIN, 0};
        int event = poll(&control, 1U, -1);
        ssize_t count;
        if (event < 0 && errno == EINTR) continue;
        if (event < 0) return;
        count = recv(channel, &release, sizeof(release), MSG_DONTWAIT | MSG_TRUNC);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count <= 0) return;
        if (count == (ssize_t)sizeof(release) && release.magic == C1_TERMINAL_IDLE_MAGIC &&
            release.kind == C1_TERMINAL_IDLE_RELEASE && release.token == token) return;
    }
}

static int idle_event(void)
{
    c1_terminal_idle_message message;
    ssize_t count;
    if (previous_hook != NULL) (void)previous_hook();
    if (channel < 0 || getpid() != owner) return 0;
    /* Bound work per Readline tick, including stale cancellation packets. */
    for (unsigned int i = 0U; i < 16U; ++i) {
        int64_t now;
        count = recv(channel, &message, sizeof(message), MSG_DONTWAIT | MSG_TRUNC);
        if (count <= 0) return 0;
        if (count != (ssize_t)sizeof(message) || message.magic != C1_TERMINAL_IDLE_MAGIC ||
            message.kind != C1_TERMINAL_IDLE_QUERY) continue;
        now = idle_now();
        if (now < 0 || now >= message.deadline_ms) continue;
        {
            sigset_t blocked, original, pending;
            bool signals_clear = true;
            sigfillset(&blocked);
            if (sigprocmask(SIG_BLOCK, &blocked, &original) != 0) continue;
            if (sigpending(&pending) != 0) signals_clear = false;
            else for (int signal_number = 1; signal_number < NSIG; ++signal_number)
                if (sigismember(&pending, signal_number) == 1) signals_clear = false;
            /* Signal traps/Readline longjmps cannot escape the parked proof. */
            message.kind = signals_clear && readline_idle() ? C1_TERMINAL_IDLE_PROOF : C1_TERMINAL_IDLE_BUSY;
            if (send(channel, &message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL) ==
                (ssize_t)sizeof(message) && message.kind == C1_TERMINAL_IDLE_PROOF)
                park_proof(message.token);
            (void)sigprocmask(SIG_SETMASK, &original, NULL);
        }
    }
    return 0;
}

static int install_idle(struct word_list *arguments)
{
    (void)arguments;
    {
        const char *image = getenv(C1_TERMINAL_IDLE_IMAGE_FD_ENV);
        if (image != NULL) {
            char *end;
            long fd;
            errno = 0;
            fd = strtol(image, &end, 10);
            if (errno || *image == '\0' || *end != '\0' || fd <= STDERR_FILENO || fd > INT_MAX)
                return 1;
            /* dlopen has mapped the image; neither the shell nor its children
             * need an open image descriptor from this point onward. */
            (void)close((int)fd);
            (void)unsetenv(C1_TERMINAL_IDLE_IMAGE_FD_ENV);
        }
    }
    if (channel < 0) {
        const char *value = getenv(C1_TERMINAL_IDLE_FD_ENV);
        char *end;
        long descriptor;
        int type;
        socklen_t size = sizeof(type);
        if ((rl_readline_version != 0x0801 && rl_readline_version != 0x0802) ||
            value == NULL || *value == '\0') return 1;
        errno = 0;
        descriptor = strtol(value, &end, 10);
        if (errno || *end != '\0' || descriptor <= STDERR_FILENO || descriptor > INT_MAX ||
            getsockopt((int)descriptor, SOL_SOCKET, SO_TYPE, &type, &size) != 0 ||
            type != SOCK_SEQPACKET) return 1;
        channel = (int)descriptor;
        owner = getpid();
        /* The initial exec needs the descriptor. Later external commands do
         * not: eliminating inherited peers also guarantees EOF on parent death. */
        if (fcntl(channel, F_SETFD, FD_CLOEXEC) != 0) { channel = -1; return 1; }
    }
    if (getpid() != owner) return 1;
    if (rl_event_hook != idle_event) {
        previous_hook = rl_event_hook;
        rl_event_hook = idle_event;
    }
    (void)rl_set_keyboard_input_timeout(10000);
    return 0;
}

void c1_terminal_idle_builtin_unload(char *name)
{
    (void)name;
    if (rl_event_hook == idle_event) rl_event_hook = previous_hook;
}

static char *const documentation[] = {
    "Install the desktop terminal's read-only idle verification hook.", NULL
};
struct builtin c1_terminal_idle_struct = {
    "c1_terminal_idle", install_idle, 1, documentation,
    "c1_terminal_idle", NULL
};
