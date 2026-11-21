/* Table pickup detector for a port without Apple's wake-gesture processor.
 * Gravity is in iOS axes (face up: z=-1; portrait: y=-1), rate in rad/s.
 * Only feed fresh, paired sensor samples. Never arm while the display is on. */
#ifndef REDWOOD_RAISE_POLICY_H
#define REDWOOD_RAISE_POLICY_H
#include <math.h>
#include <stdbool.h>

struct redwood_raise {
    double last, rest_at, armed_at, moving_at, viewing_at, cooldown;
    double baseline[3];
    bool resting, armed;
};

static bool
redwood_raise_sample(struct redwood_raise *p, double now, bool allowed,
    double x, double y, double z, double speed, double acceleration)
{
    double cooldown = p->cooldown;
    if (!isfinite(now) || !isfinite(x) || !isfinite(y) || !isfinite(z) ||
        !isfinite(speed) || !isfinite(acceleration)) return false;
    if (!allowed || (p->last && (now <= p->last || now - p->last > .75))) {
        *p = (struct redwood_raise){ .last = now, .cooldown = cooldown };
        return false;
    }
    p->last = now;
    if (now < cooldown) return false;
    double norm = sqrt(x*x + y*y + z*z);
    if (speed > .2 && p->armed && !p->moving_at) p->moving_at = now;
    if (p->moving_at && now - p->moving_at > 2.5) {
        p->armed = p->resting = false;
        return false;
    }
    if (norm < .8 || norm > 1.2 || acceleration > .8) {
        /* A pickup starts with linear acceleration. Ignore that transient,
         * but keep a previously established table pose until it times out. */
        p->resting = false;
        p->viewing_at = 0;
        return false;
    }
    x /= norm; y /= norm; z /= norm;
    /* Require a face-up resting pose first. A pocket, a face-down phone,
     * a phone held upright, and walking alone do not arm this detector. */
    if (!p->armed) {
        if (z < -.8 && fabs(y) < .55 && speed < .15 && acceleration < .12) {
            if (!p->resting) { p->rest_at = now; p->resting = true; }
            if (now - p->rest_at >= .5) {
                p->baseline[0]=x; p->baseline[1]=y; p->baseline[2]=z;
                p->armed = true; p->armed_at = now;
                p->moving_at = p->viewing_at = 0;
            }
        } else p->resting = false;
        return false;
    }
    double dot = x*p->baseline[0] + y*p->baseline[1] + z*p->baseline[2];
    bool viewing = p->moving_at && dot < .87 && y < -.45 && z < -.15 && fabs(x) < .65;
    if (!viewing) { p->viewing_at = 0; return false; }
    if (!p->viewing_at) p->viewing_at = now;
    if (now - p->viewing_at < .15) return false;
    *p = (struct redwood_raise){ .last = now, .cooldown = now + 3 };
    return true;
}
#endif
