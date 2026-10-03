#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
/* Production power gate + real host PTY. This executable never links poweroff.c. */
#include "../src/hal/linux/ui_runtime.c"
#include <assert.h>

static bool run_active, lease_active;
static int update_error;
static enum c1_update_phase update_phase = C1_UPDATE_CONFIRMED;
static unsigned deferrals;
static char deferred_reason[64];

bool c1_app_run_active(void) { return run_active; }
bool c1_app_lease_active(void) { return lease_active; }
int c1_update_state_load(const char *root, struct c1_update_state *state,
                         char *error, size_t capacity)
{
    (void)root; (void)error; (void)capacity;
    memset(state, 0, sizeof(*state));
    state->phase = update_phase;
    return update_error;
}

static c1_status capture(void *context, const c1_record *record)
{
    (void)context;
    assert(strcmp(record->event, "shutdown-deferred") == 0);
    ++deferrals;
    for (size_t i = 0; i < record->field_count; ++i)
        if (strcmp(record->fields[i].name, "reason") == 0)
            snprintf(deferred_reason, sizeof(deferred_reason), "%s", record->fields[i].value.text);
    return C1_STATUS_OK;
}

static void pause_tick(void) { (void)poll(NULL, 0, 10); }

static void wait_for_prompt(c1_terminal_session *session)
{
    char buffer[512];
    bool seen = false;
    int64_t deadline = monotonic_milliseconds() + 3000;
    while (!seen && monotonic_milliseconds() < deadline) {
        ssize_t count = c1_terminal_read(session, buffer, sizeof(buffer) - 1U);
        assert(count >= 0);
        if (count > 0) {
            buffer[count] = '\0';
            seen = strstr(buffer, "# ") != NULL || strstr(buffer, "$ ") != NULL;
        }
        pause_tick();
    }
    assert(seen && c1_terminal_is_running(session));
}

static bool wait_for_idle_close(c1_service_worker *worker, c1_terminal_session *user,
                                c1_terminal_session *app, c1_record_sink sink)
{
    int64_t deadline = monotonic_milliseconds() + 3000;
    do {
        if (prepare_automatic_shutdown(worker, user, app, sink)) return true;
        pause_tick();
    } while (monotonic_milliseconds() < deadline);
    return false;
}

static void wait_for_output(c1_terminal_session *session, const char *needle)
{
    char output[4096] = "";
    size_t used = 0U;
    int64_t deadline = monotonic_milliseconds() + 5000;
    while (monotonic_milliseconds() < deadline) {
        ssize_t count = c1_terminal_read(session, output + used, sizeof(output) - used - 1U);
        assert(count >= 0);
        if (count > 0) {
            used += (size_t)count;
            output[used] = '\0';
            if (strstr(output, needle) != NULL) return;
            if (used > sizeof(output) / 2U) {
                memmove(output, output + used / 2U, used - used / 2U);
                used -= used / 2U;
            }
        }
        pause_tick();
    }
    assert(!"terminal command did not finish within test deadline");
}

static void test_completed_commands_release_power_gate(void)
{
    c1_service_worker worker = {.pid = -1, .fd = -1, .cancel_fd = -1};
    c1_record_sink sink = {capture, NULL};
    /* Exercise the actual automatic power gate, not just the terminal API.
     * A command's text is not proof: wait for a marker split in its printf. */
    const char *commands[] = {
        "ls / >/dev/null; printf 'C1_%s\\n' LS_DONE\r",
        "pwd >/dev/null; echo finished >/dev/null; printf 'C1_%s\\n' AGAIN_DONE\r"
    };
    const char *markers[] = {"C1_LS_DONE", "C1_AGAIN_DONE"};
    for (unsigned cycle = 0U; cycle < 2U; ++cycle) {
        c1_terminal_session user, app;
        c1_terminal_init(&user);
        c1_terminal_init(&app);
        assert(c1_terminal_start(&user, 37, 8) == C1_STATUS_OK);
        wait_for_prompt(&user);
        for (size_t command = 0U; command < sizeof(commands) / sizeof(commands[0]); ++command) {
            assert(c1_terminal_write(&user, commands[command], strlen(commands[command])) == C1_STATUS_OK);
            wait_for_output(&user, markers[command]);
        }
        assert(wait_for_idle_close(&worker, &user, &app, sink));
        assert(automatic_shutdown_safe(&worker, &user, &app));
        c1_terminal_stop(&user);
        c1_terminal_stop(&app);
    }
}

static void test_delayed_close_keeps_input_queued(void)
{
    c1_terminal_session user, app;
    c1_terminal_screen screen = {0};
    c1_terminal_init(&user);
    c1_terminal_init(&app);
    assert(c1_terminal_start(&user, 37, 8) == C1_STATUS_OK);
    wait_for_prompt(&user);
    assert(kill(user.child_pid, SIGSTOP) == 0);
    int stopped;
    assert(waitpid(user.child_pid, &stopped, WUNTRACED) == user.child_pid && WIFSTOPPED(stopped));
    assert(!c1_terminal_close_idle(&user));
    assert(user.idle_close_pending && !idle_close_settled(&user));
    memcpy(screen.reply, "x", 1U);
    screen.reply_length = 1U;
    assert(flush_terminal_replies(&user, &screen) == C1_STATUS_UNAVAILABLE);
    assert(screen.reply_length == 1U && screen.reply_offset == 0U && screen.reply[0] == 'x');
    /* An independently stopped shell makes the supervisor reject its request.
     * It must not resume a stop it did not own or turn the refusal into input loss. */
    assert(kill(user.shell_pid, SIGSTOP) == 0);
    int64_t deadline = monotonic_milliseconds() + 3000;
    bool shell_stopped = false;
    while (!shell_stopped && monotonic_milliseconds() < deadline) {
        char path[64], data[4096];
        snprintf(path, sizeof(path), "/proc/%ld/stat", (long)user.shell_pid);
        FILE *file = fopen(path, "r");
        assert(file != NULL && fgets(data, sizeof(data), file) != NULL);
        assert(fclose(file) == 0);
        char *end = strrchr(data, ')');
        shell_stopped = end != NULL && end[1] == ' ' && end[2] == 'T';
        if (!shell_stopped) pause_tick();
    }
    assert(shell_stopped && kill(user.child_pid, SIGCONT) == 0);
    while (!idle_close_settled(&user) && monotonic_milliseconds() < deadline) pause_tick();
    assert(!user.idle_close_pending && c1_terminal_is_running(&user));
    assert(kill(user.shell_pid, SIGCONT) == 0);
    assert(flush_terminal_replies(&user, &screen) == C1_STATUS_OK);
    assert(screen.reply_length == 0U && user.input_seen);
    c1_terminal_stop(&user);
    c1_terminal_stop(&app);
}

static void test_only_foreground_command_blocks(void)
{
    c1_service_worker worker = {.pid = -1, .fd = -1, .cancel_fd = -1};
    c1_terminal_session user, app;
    c1_record_sink sink = {capture, NULL};
    int64_t deadline;
    c1_terminal_init(&user);
    c1_terminal_init(&app);
    assert(c1_terminal_start(&user, 37, 8) == C1_STATUS_OK);
    wait_for_prompt(&user);
    assert(automatic_shutdown_safe(&worker, &user, &app));
    assert(c1_terminal_write(&user, "sleep 30\r", 9U) == C1_STATUS_OK);
    deadline = monotonic_milliseconds() + 3000;
    while (!c1_terminal_command_running(&user) && monotonic_milliseconds() < deadline)
        pause_tick();
    assert(c1_terminal_command_running(&user));
    assert(!automatic_shutdown_safe(&worker, &user, &app));
    assert(!prepare_automatic_shutdown(&worker, &user, &app, sink));
    assert(strcmp(deferred_reason, "terminal-command-running") == 0);
    c1_terminal_stop(&user);

    c1_terminal_init(&user);
    assert(c1_terminal_start(&user, 37, 8) == C1_STATUS_OK);
    wait_for_prompt(&user);
    assert(c1_terminal_write(&user, "unsubmitted", 11U) == C1_STATUS_OK);
    assert(!c1_terminal_command_running(&user));
    assert(prepare_automatic_shutdown(&worker, &user, &app, sink));
    assert(user.child_pid <= 0);

    c1_terminal_init(&user);
    assert(c1_terminal_start(&user, 37, 8) == C1_STATUS_OK);
    wait_for_prompt(&user);
    const char background[] = "sleep 30 & printf 'C1_BG_DONE\\n'\r";
    assert(c1_terminal_write(&user, background, strlen(background)) == C1_STATUS_OK);
    wait_for_output(&user, "C1_BG_DONE");
    assert(!c1_terminal_command_running(&user));
    assert(prepare_automatic_shutdown(&worker, &user, &app, sink));
    assert(user.child_pid <= 0);
    c1_terminal_stop(&app);
}

int main(void)
{
    c1_service_worker worker = {.pid = -1, .fd = -1, .cancel_fd = -1};
    c1_terminal_session user, app;
    c1_record_sink sink = {capture, NULL};
    c1_power_policy policy;
    c1_terminal_init(&user);
    c1_terminal_init(&app);
    desktop_job.pid = network_clock.pid = -1;
    assert(flush_terminal_replies(NULL, NULL) == C1_STATUS_OK);
    assert(prepare_automatic_shutdown(&worker, &user, &app, sink));

    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        assert(c1_terminal_start(&user, 37, 8) == C1_STATUS_OK);
        wait_for_prompt(&user);
        worker.pid = 123;
        assert(automatic_shutdown_safe(&worker, &user, &app));
        worker.pid = -1;
        run_active = true;
        assert(automatic_shutdown_safe(&worker, &user, &app));
        run_active = false;
        lease_active = true;
        assert(automatic_shutdown_safe(&worker, &user, &app));
        lease_active = false;
        update_error = -1;
        assert(automatic_shutdown_safe(&worker, &user, &app));
        update_error = 0;
        update_phase = C1_UPDATE_DOWNLOADING;
        assert(automatic_shutdown_safe(&worker, &user, &app));
        update_phase = C1_UPDATE_CONFIRMED;
        app.child_pid = 123;
        assert(automatic_shutdown_safe(&worker, &user, &app));
        app.child_pid = -1;
        c1_power_policy_init(&policy, 0);
        c1_power_policy_configure(&policy, 180000, 0, 300000);
        c1_power_policy_set_external_power(&policy, true, false, 0);
        assert(c1_power_policy_tick(&policy, 180000) == C1_POWER_ACTION_ENTER_LOCK);
        assert(c1_power_policy_tick(&policy, 480000) == C1_POWER_ACTION_SHUTDOWN);
        assert(wait_for_idle_close(&worker, &user, &app, sink));
        assert(user.child_pid <= 0 && automatic_shutdown_safe(&worker, &user, &app));
    }
    assert(deferrals == 0);
    c1_terminal_stop(&user);
    c1_terminal_stop(&app);
    test_only_foreground_command_blocks();
    test_completed_commands_release_power_gate();
    test_delayed_close_keeps_input_queued();
    puts("automatic shutdown tests passed: only a foreground command defers; input, background jobs and external work do not (no poweroff)");
    return 0;
}
