#!/usr/bin/env python3
"""Compile the kernel's actual cap policy on the host and exercise its combinations."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'xnu/files/bsd/dev/redwood_exynos_acpm.c').read_text()

def function(name):
    match = re.search(r'static (?:uint32_t|int)\n' + name + r'\([^)]*\)\n\{', source)
    assert match, name
    end = source.index('\n}', match.end()) + 2
    return source[match.start():end]

start = source.index('enum { D_MIF')
end = source.index('\nstatic const char *const zones', start)
tables = source[start:end]
program = '''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define os_atomic_load(p, order) (*(p))
static unsigned efficiency_mask;
static int request_result, request_calls;
static int set_rate(unsigned domain, uint32_t khz) {
    (void)domain; (void)khz; request_calls++; return request_result;
}
static int efficiency_auto = 1;
static int efficiency, redwood_screen_on = 1, redwood_lowpower;
static unsigned prof_sleep = 1, prof_lpm = 1;
enum { PROF_NORMAL, PROF_LPM, PROF_OFF };
''' + tables + '\n' + '\n'.join(function(n) for n in (
    'efficiency_cap', 'efficiency_active', 'power_profile', 'profile_cap', 'governor_set_rate')) + r'''
int main(void) {
    uint32_t sent = 2730000;
    request_result = 1;
    assert(governor_set_rate(4, 1794000, &sent) == -1);
    assert(sent == 2730000 && request_calls == 1);
    request_result = 0;
    assert(governor_set_rate(4, 1794000, &sent) == 1);
    assert(sent == 1794000 && request_calls == 2);
    assert(governor_set_rate(4, 1794000, &sent) == 0);
    assert(request_calls == 2); /* no repeated ACPM request for an unchanged cap */
    assert(domains[0].step1 == 82 && domains[0].step2 == 90);
    assert(domains[1].step1 == 82 && domains[1].step2 == 90);
    assert(domains[2].step1 == 78 && domains[2].step2 == 86);
    assert(domains[2].mid == 1586000);
    assert(domains[3].step1 == 82 && domains[3].step2 == 90);
    const uint32_t expected[] = {1456000, 2106000, 1794000, 572000, 1352000, 0, 0};
    for (unsigned d = 0; d < NDOMAINS; d++) {
        efficiency = 0;
        assert(profile_cap(d, domains[d].target, PROF_NORMAL) == domains[d].target);
        efficiency = 1;
        assert(efficiency_cap(domains[d].domain) == expected[d]);
        if (expected[d]) assert(profile_cap(d, domains[d].target, PROF_NORMAL) == expected[d]);
        for (int prof = PROF_NORMAL; prof <= PROF_OFF; prof++) {
            efficiency = 0;
            uint32_t baseline = profile_cap(d, domains[d].target, prof);
            efficiency = 1;
            uint32_t capped = profile_cap(d, domains[d].target, prof);
            assert(!baseline || capped <= baseline);
            if (expected[d]) assert(capped <= expected[d]);
            /* A lower explicit target or thermal cap must never be raised. */
            assert(profile_cap(d, 100000, prof) <= 100000);
        }
        efficiency = 0;
        assert(profile_cap(d, domains[d].target, PROF_NORMAL) == domains[d].target);
    }
    /* Each independent bit must cap only its own domain, across all profiles. */
    efficiency = 0;
    for (unsigned selected = 0; selected < 5; selected++) {
        efficiency_mask = 1u << selected;
        for (unsigned d = 0; d < NDOMAINS; d++) {
            for (int prof = PROF_NORMAL; prof <= PROF_OFF; prof++) {
                efficiency_auto = 0;
                uint32_t base = profile_cap(d, domains[d].target, prof);
                efficiency_auto = 1;
                uint32_t actual = profile_cap(d, domains[d].target, prof);
                uint32_t want = selected < 4 && d == selected && expected[d] && (!base || expected[d] < base) ? expected[d] : base;
                assert(actual == want);
                assert(profile_cap(d, 100000, prof) <= 100000);
            }
        }
    }
    efficiency_mask = 0;
    efficiency = 1;
    redwood_lowpower = 1;
    assert(power_profile() == PROF_LPM);
    assert(profile_cap(2, 2730000, power_profile()) == 1586000);
    redwood_screen_on = 0;
    assert(power_profile() == PROF_OFF);
    assert(profile_cap(4, 2028000, power_profile()) == 421000);
    assert(profile_cap(3, 800000, power_profile()) == 377000);
    puts("PASS: efficiency caps, normal restoration, thermal/target limits, LPM and screen-off precedence");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile = Path(tmp) / 'policy.c'
    binary = Path(tmp) / 'policy'
    cfile.write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
