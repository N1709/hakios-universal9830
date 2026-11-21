/*
 * The debug port (kern.redwood.poke): experiments on the running phone without a reboot. See bsd/dev/redwood_dbgport.c.
 * A driver adds its own commands: redwood_dbgport_add("snd.send", "ID TYPE [a..]: one message to Calliope", fn).
 * fn gets the numbers after the command (decimal or 0x hex) and writes its answer into out (cap bytes).
 */
#ifndef _SYS_REDWOOD_DBGPORT_H_
#define _SYS_REDWOOD_DBGPORT_H_
#include <stdint.h>
#include <stddef.h>

typedef void (*redwood_dbgport_fn)(const uint64_t *v, unsigned n, char *out, size_t cap);
int redwood_dbgport_add(const char *name, const char *help, redwood_dbgport_fn fn);
#endif
