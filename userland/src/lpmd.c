/* Settings Low Power Mode -> kernel, driven only by notifications.
 * Independent thermal control belongs to the kernel's existing ACPM loop.
 * No temperature polling, log buffer, timer or whole-device thermal fallback.
 */
#include <dispatch/dispatch.h>
#include <notify.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

#define LPM_NAME "com.apple.system.lowpowermode"

static int
get_int(const char *name)
{
    int value = -1;
    size_t len = sizeof(value);
    return sysctlbyname(name, &value, &len, NULL, 0) == 0 ? value : -1;
}

static int
lpm_state(int token)
{
    uint64_t state = 0;
    return notify_get_state(token, &state) == NOTIFY_STATUS_OK ? state != 0 : -1;
}

static void
settings_sync(int token)
{
    int manual = lpm_state(token);
    if (manual < 0 || get_int("kern.redwood.lowpower") == manual) return;
    if (sysctlbyname("kern.redwood.lowpower", NULL, NULL, &manual, sizeof(manual)) != 0) {
        perror("kern.redwood.lowpower");
    } else {
        printf("lpmd: Settings Low Power Mode %s\n", manual ? "on" : "off");
        fflush(stdout);
    }
}

int
main(int argc, char **argv)
{
    int token = 0;
    if (notify_register_check(LPM_NAME, &token) != NOTIFY_STATUS_OK) return 1;
    if (argc > 1 && strcmp(argv[1], "probe") == 0) {
        printf("lpmd: Settings=%d kernel-lowpower=%d efficiency=%d native-auto=%d mask=%d\n",
            lpm_state(token), get_int("kern.redwood.lowpower"), get_int("kern.redwood.efficiency"),
            get_int("kern.redwood.efficiency_auto"), get_int("kern.redwood.efficiency_mask"));
        notify_cancel(token);
        return 0;
    }
    printf("lpmd: event-driven Settings mirror; native cooling %s\n",
        get_int("kern.redwood.efficiency_auto") >= 0 ? "available" : "requires updated kernel");
    fflush(stdout);
    settings_sync(token);
    notify_cancel(token);
    if (notify_register_dispatch(LPM_NAME, &token, dispatch_get_main_queue(), ^(int t) {
        settings_sync(t);
    }) != NOTIFY_STATUS_OK) return 1;
    settings_sync(token); /* cover a change during registration */
    dispatch_main();
}
