/* Read-only status by default. --isolated-test uses temporary non-system bags
 * and hard-coded TEST credentials, never changes the device's system bag. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static io_connect_t connection;
static kern_return_t
scalar(unsigned selector, const uint64_t *input, unsigned n, uint64_t *out, uint32_t *count)
{
    return IOConnectCallScalarMethod(connection, selector, input, n, out, count);
}
static kern_return_t
unlock(unsigned bag, const char *secret)
{
    uint64_t args[] = {bag, (uint64_t)(uintptr_t)secret, strlen(secret)};
    return scalar(9, args, 3, NULL, NULL);
}
static int
state(unsigned bag, uint64_t *value)
{
    uint64_t input = bag;
    uint32_t n = 1;
    return scalar(7, &input, 1, value, &n) == KERN_SUCCESS && n == 1;
}
static int
change(unsigned bag, const char *old, const char *newcode)
{
    uint64_t args[] = {bag, 0, 255, (uint64_t)(uintptr_t)old, strlen(old),
        (uint64_t)(uintptr_t)newcode, strlen(newcode), 0,0,0,0}, result;
    uint32_t n = 1;
    return scalar(15, args, 11, &result, &n) == KERN_SUCCESS && n == 1 && result == bag;
}
int main(int argc, char **argv)
{
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleKeyStore"));
    if (!service) { fputs("AppleKeyStore not found\n", stderr); return 1; }
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (kr) { fprintf(stderr, "open failed: %#x\n", kr); return 1; }
    uint64_t before = 0, after = 0;
    if (!state(0, &before)) return 1;
    printf("system state flags: %#llx (1 locked, 2 no-passcode, 4 first-unlock)\n", (unsigned long long)before);
    if (argc < 2 || strcmp(argv[1], "--isolated-test")) { IOServiceClose(connection); return 0; }
    uint64_t created = 0, loaded = 0;
    int result = 1;
    uint64_t args[] = {0, (uint64_t)-1, 0, 0, 0, 0};
    uint32_t n = 1;
    kr = scalar(2, args, 6, &created, &n);
    if (kr || !created || created > 15) {
        fprintf(stderr, "temporary bag unavailable: %#x; experimental backend may be disabled\n", kr);
        goto done;
    }
#define CHECK(expr, label) do { if (!(expr)) { fprintf(stderr, "FAIL: %s\n", label); goto done; } } while (0)
    CHECK(change((unsigned)created, "", "probe-only-A"), "set temporary passcode");
    uint64_t bag = created, flags;
    CHECK(scalar(8, &bag, 1, NULL, NULL) == KERN_SUCCESS, "lock temporary bag");
    CHECK(unlock((unsigned)created, "wrong") != KERN_SUCCESS, "wrong code rejected");
    CHECK(state((unsigned)created, &flags) && (flags & 1), "wrong code leaves bag locked");
    CHECK(unlock((unsigned)created, "probe-only-A") == KERN_SUCCESS, "right code accepted");
    CHECK(change((unsigned)created, "probe-only-A", "probe-only-B"), "change temporary passcode");
    uint8_t record[256] = {0};
    size_t size = sizeof(record);
    CHECK(IOConnectCallMethod(connection, 3, &bag, 1, NULL, 0, NULL, NULL, record, &size) == KERN_SUCCESS,
        "save temporary bag");
    n = 1;
    CHECK(IOConnectCallMethod(connection, 6, NULL, 0, record, size, &loaded, &n, NULL, NULL) == KERN_SUCCESS &&
        loaded && loaded != created && loaded <= 15, "load temporary bag");
    CHECK(state((unsigned)loaded, &flags) && flags == 1, "loaded bag starts locked before first unlock");
    CHECK(unlock((unsigned)loaded, "probe-only-A") != KERN_SUCCESS, "old code rejected after reload");
    CHECK(unlock((unsigned)loaded, "probe-only-B") == KERN_SUCCESS, "new code accepted after reload");
    CHECK(change((unsigned)loaded, "probe-only-B", ""), "remove temporary passcode");
    CHECK(state((unsigned)loaded, &flags) && flags == 6, "removed passcode returns disabled state");
    result = 0;
done:
    if (created && created <= 15) scalar(4, &created, 1, NULL, NULL);
    if (loaded && loaded <= 15) scalar(4, &loaded, 1, NULL, NULL);
    if (!state(0, &after) || after != before) {
        fprintf(stderr, "system state changed during test; inspect before proceeding\n"); result = 1;
    }
    IOServiceClose(connection);
    if (!result) puts("PASS: isolated native bag test; system state unchanged");
    return result;
}
