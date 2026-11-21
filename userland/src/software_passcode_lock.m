#include "software_passcode_lock.h"
__attribute__((constructor)) static void start(void) { software_passcode_lock_start(); }
