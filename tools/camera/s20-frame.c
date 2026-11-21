/* Download the last successful bounded S20 camera diagnostic frame to stdout.
 * Build for iOS; run as root. The RWCAM001 result is decoded by tools/camraw.py. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>

int main(void)
{
    const char *name = "kern.redwood.camera_frame";
    size_t size = 0;
    if (sysctlbyname(name, NULL, &size, NULL, 0) || size < 4096 || size > 8 * 1024 * 1024) {
        perror("camera frame size");
        return 1;
    }
    void *data = malloc(size);
    if (!data) return 1;
    if (sysctlbyname(name, data, &size, NULL, 0)) {
        perror("camera frame");
        free(data);
        return 1;
    }
    int ok = fwrite(data, 1, size, stdout) == size && fflush(stdout) == 0;
    free(data);
    return ok ? 0 : 1;
}
