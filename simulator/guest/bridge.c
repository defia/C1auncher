#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/pidfd.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FRAME_BYTES 5624
struct screen_packet { uint32_t sequence, full; unsigned char pixels[FRAME_BYTES]; };
struct control_packet { uint16_t source, code; int32_t value; };
static int connection = -1, device = -1, logs = -1;
static pid_t child = -1;
static char app[64] = "none";
static char input_app[64] = "";
static const char hex[] = "0123456789abcdef";

static void send_all(const char *data, size_t size)
{
    while (size) {
        ssize_t count = send(connection, data, size, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) exit(1);
        data += count; size -= (size_t)count;
    }
}

static void send_text(const char *data) { send_all(data, strlen(data)); }

static int descendant(pid_t pid)
{
    for (int depth = 0; depth < 64 && pid > 1; ++depth) {
        char path[96], line[256];
        int parent = 0;
        if (pid == child) return 1;
        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        FILE *file = fopen(path, "r");
        if (!file) return 0;
        while (fgets(line, sizeof(line), file)) if (sscanf(line, "PPid: %d", &parent) == 1) break;
        fclose(file);
        if (parent == pid) return 0;
        pid = (pid_t)parent;
    }
    return 0;
}

/* The launcher remains our child while a store app owns its own evdev input. */
static void report_input_app(void)
{
    char current[64];
    snprintf(current, sizeof(current), "%s", app);
    DIR *processes = !strcmp(app, "launcher") && child > 0 ? opendir("/proc") : NULL;
    struct dirent *process;
    while (processes && (process = readdir(processes))) {
        pid_t pid = (pid_t)atoi(process->d_name);
        char path[512], executable[512], target[512];
        const char prefix[] = "/storage/c1/apps/";
        if (pid <= 1 || pid == child) continue;
        snprintf(path, sizeof(path), "/proc/%d/exe", pid);
        ssize_t size = readlink(path, executable, sizeof(executable) - 1);
        if (size <= 0) continue;
        executable[size] = 0;
        if (strncmp(executable, prefix, sizeof(prefix) - 1) || !descendant(pid)) continue;
        char *id = executable + sizeof(prefix) - 1, *end = strchr(id, '/');
        if (!end || end == id || (size_t)(end - id) >= sizeof(current)) continue;
        snprintf(path, sizeof(path), "/proc/%d/fd", pid);
        DIR *descriptors = opendir(path);
        struct dirent *descriptor;
        int owns_input = 0;
        while (descriptors && (descriptor = readdir(descriptors))) {
            if (descriptor->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "/proc/%d/fd/%s", pid, descriptor->d_name);
            size = readlink(path, target, sizeof(target) - 1);
            if (size <= 0) continue;
            target[size] = 0;
            if (!strcmp(target, "/dev/input/event0") || !strcmp(target, "/dev/input/event1")) { owns_input = 1; break; }
        }
        if (descriptors) closedir(descriptors);
        if (owns_input) {
            memcpy(current, id, (size_t)(end - id)); current[end - id] = 0;
            break;
        }
    }
    if (processes) closedir(processes);
    if (strcmp(input_app, current)) {
        char message[96];
        snprintf(input_app, sizeof(input_app), "%s", current);
        snprintf(message, sizeof(message), "INPUT %s\n", input_app);
        send_text(message);
    }
}

static void send_hex(const char *prefix, const unsigned char *data, size_t size)
{
    char buffer[12000];
    size_t used = (size_t)snprintf(buffer, sizeof(buffer), "%s ", prefix);
    if (used + size * 2 + 1 > sizeof(buffer)) return;
    for (size_t i = 0; i < size; ++i) {
        buffer[used++] = hex[data[i] >> 4]; buffer[used++] = hex[data[i] & 15];
    }
    buffer[used++] = '\n'; send_all(buffer, used);
}

/* Capture descendants with pidfds before stopping their parents. Some Go apps
 * call ReturnToDesktop(), which sends SIGKILL to their current parent. Do not
 * adopt those apps while their original parent is being terminated. */
static void stop_app(void)
{
    struct owned_process { int fd, adopted; } *owned = NULL;
    size_t count = 0;
    int detached = prctl(PR_SET_CHILD_SUBREAPER, 0) == 0;
    DIR *directory = opendir("/proc");
    struct dirent *entry;
    while (directory && (entry = readdir(directory))) {
        pid_t pid = (pid_t)atoi(entry->d_name), ancestor = pid;
        int direct_parent = 0, ours = 0;
        if (pid <= 1 || pid == getpid()) continue;
        int fd = pidfd_open(pid, 0);
        if (fd < 0) continue;
        for (int depth = 0; depth < 64 && ancestor > 1; ++depth) {
            char path[96], line[256];
            int parent = 0;
            snprintf(path, sizeof(path), "/proc/%d/status", ancestor);
            FILE *file = fopen(path, "r");
            if (!file) break;
            while (fgets(line, sizeof(line), file)) if (sscanf(line, "PPid: %d", &parent) == 1) break;
            fclose(file);
            if (!depth) direct_parent = parent;
            if (parent == getpid()) { ours = 1; break; }
            if (parent == ancestor) break;
            ancestor = (pid_t)parent;
        }
        if (!ours) { close(fd); continue; }
        struct owned_process *next = realloc(owned, (count + 1) * sizeof(*owned));
        if (!next) { pidfd_send_signal(fd, SIGKILL, NULL, 0); close(fd); continue; }
        owned = next;
        owned[count++] = (struct owned_process){fd, direct_parent == getpid() && pid != child};
    }
    if (directory) closedir(directory);
    /* Already orphaned children have lost the parent their exit hook expects. */
    for (size_t i = 0; i < count; ++i)
        if (owned[i].adopted || !detached) pidfd_send_signal(owned[i].fd, SIGKILL, NULL, 0);
    if (child > 0) kill(-child, detached ? SIGTERM : SIGKILL);
    for (int pass = 0; pass < 3; ++pass) {
        for (size_t i = 0; i < count; ++i)
            pidfd_send_signal(owned[i].fd, pass || !detached ? SIGKILL : SIGTERM, NULL, 0);
        usleep(100000);
        while (waitpid(-1, NULL, WNOHANG) > 0) { }
    }
    for (size_t i = 0; i < count; ++i) close(owned[i].fd);
    free(owned);
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) { perror("restore child subreaper"); exit(1); }
    child = -1;
    if (logs >= 0) close(logs);
    logs = -1;
}

static int allowed(const char *id)
{
    static const char *const ids[] = { "launcher", "hello", "book-reader", "pic", "chichugames", "music-player", "pinao", "custom", NULL };
    for (int i = 0; ids[i]; ++i) if (!strcmp(id, ids[i])) return 1;
    return 0;
}

static void start_app(const char *id)
{
    int pipefd[2];
    char message[160];
    if (!allowed(id)) { send_text("ERROR unknown-app\n"); return; }
    stop_app();
    if (pipe2(pipefd, O_CLOEXEC) < 0) return;
    child = fork();
    if (!child) {
        setsid();
        dup2(pipefd[1], STDOUT_FILENO); dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]); close(connection); close(device);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); close(nullfd); }
        setenv("PATH", "/usr/data/c1/bin:/sbin:/usr/sbin:/bin:/usr/bin", 1);
        setenv("HOME", "/root", 1); setenv("TERM", "xterm-256color", 1);
        if (!strcmp(id, "custom")) { chdir("/opt/custom"); execl("/opt/custom/app", "app", NULL); }
        else if (!strcmp(id, "launcher")) execl("/usr/data/c1/bin/C1ancher", "C1ancher", "app", NULL);
        else execl("/usr/data/c1/bin/c1pkg", "c1pkg", "launch", id, NULL);
        perror("exec original MIPS binary"); _exit(127);
    }
    close(pipefd[1]);
    if (child < 0) { close(pipefd[0]); return; }
    logs = pipefd[0]; fcntl(logs, F_SETFL, O_NONBLOCK);
    snprintf(app, sizeof(app), "%s", id);
    int size = snprintf(message, sizeof(message), "APP %s %d running\n", app, child);
    send_all(message, (size_t)size);
}

static void command(char *line)
{
    unsigned source, code;
    int value;
    struct control_packet packet;
    char id[64];
    if (sscanf(line, "KEY %u %u %d", &source, &code, &value) == 3 && source < 2 && code <= 767 && value >= 0 && value <= 2) {
        packet = (struct control_packet){(uint16_t)source, (uint16_t)code, value};
        if (write(device, &packet, sizeof(packet)) != sizeof(packet)) perror("key");
    } else if (sscanf(line, "BATTERY %d %u", &value, &code) == 2 && value >= 0 && value <= 100 && code <= 1) {
        packet = (struct control_packet){2, (uint16_t)code, value};
        if (write(device, &packet, sizeof(packet)) != sizeof(packet)) perror("battery");
    } else if (sscanf(line, "RUN %63s", id) == 1) start_app(id);
    else if (!strcmp(line, "STOP")) {
        stop_app(); send_text("APP none 0 stopped\n");
    } else if (!strcmp(line, "SHUTDOWN")) {
        stop_app();
        sync();
        umount("/usr/data"); umount("/storage"); umount("/persist");
        sync();
        send_text("BYE synced\n");
    }
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 7654;
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    char pending[4096]; size_t pending_size = 0;
    if (port < 1 || port > 65535) return 2;
    signal(SIGPIPE, SIG_IGN);
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    inet_pton(AF_INET, "10.0.2.2", &address.sin_addr);
    device = open("/dev/c1sim", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (device < 0) { perror("c1sim device"); return 1; }
    for (;;) {
        connection = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (connection < 0) return 1;
        if (!connect(connection, (struct sockaddr *)&address, sizeof(address))) break;
        close(connection); sleep(1);
    }
    send_text("READY c1sim-1 mipsel-o32\n");
    start_app("launcher");
    for (;;) {
        report_input_app();
        struct pollfd fds[] = { {connection, POLLIN, 0}, {device, POLLIN, 0}, {logs, POLLIN, 0} };
        if (poll(fds, 3, 250) < 0 && errno != EINTR) break;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (fds[0].revents & POLLIN) {
            ssize_t count = recv(connection, pending + pending_size, sizeof(pending) - pending_size - 1, 0);
            if (count <= 0) break;
            pending_size += (size_t)count; pending[pending_size] = 0;
            char *start = pending, *end;
            while ((end = strchr(start, '\n'))) { *end = 0; command(start); start = end + 1; }
            pending_size -= (size_t)(start - pending);
            memmove(pending, start, pending_size);
            if (pending_size == sizeof(pending) - 1) break;
        }
        if (fds[1].revents & POLLIN) {
            struct screen_packet packet;
            if (read(device, &packet, sizeof(packet)) == sizeof(packet)) {
                char prefix[64]; snprintf(prefix, sizeof(prefix), "FRAME %u %u", packet.sequence, packet.full);
                send_hex(prefix, packet.pixels, FRAME_BYTES);
            }
        }
        if (logs >= 0 && fds[2].fd == logs && (fds[2].revents & (POLLIN | POLLHUP))) {
            unsigned char buffer[2048]; ssize_t count;
            while ((count = read(logs, buffer, sizeof(buffer))) > 0) {
                char prefix[96]; snprintf(prefix, sizeof(prefix), "LOG %s", app);
                send_hex(prefix, buffer, (size_t)count);
            }
        }
        int status; pid_t ended;
        while ((ended = waitpid(-1, &status, WNOHANG)) > 0) {
            if (ended == child) {
                char message[160];
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                int size = snprintf(message, sizeof(message), "EXIT %s %d\n", app, code);
                send_all(message, (size_t)size); child = -1;
                if (logs >= 0) close(logs);
                logs = -1;
            }
        }
    }
    stop_app(); close(device); close(connection);
    return 0;
}
