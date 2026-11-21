/* Optional loadable form of the same home-wallpaper effect built into the
 * SpringBoard bridge. Lets the effect be updated with a SpringBoard restart. */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "wallpaper_motion.h"

__attribute__((constructor)) static void
redwood_wallpaper_init(void)
{
    const char *name=getprogname();
    if(name && !strcmp(name,"SpringBoard")) wallpaper_motion_start();
}
