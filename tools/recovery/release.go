// SPDX-License-Identifier: MIT
package main

// TWRP on a phone running an Android ROM (ExtremeROM, UN1CA, LineageOS...) maps
// super's logical partitions through device-mapper and mounts /data, often via
// dm-default-key. Those holds block exclusive writes. planRelease lists exactly
// what must be unmounted and which dm devices to remove, and refuses anything
// that also depends on a device outside the installer's targets.

import (
	"errors"
	"fmt"
	"sort"
	"strconv"
	"strings"
)

// blockView abstracts /sys/class/block so the planner is testable on any host.
type blockView interface {
	Holders(dev string) ([]string, error) // kernel names holding dev (e.g. dm-3)
	Slaves(dev string) ([]string, error)  // kernel names dev is built on
	ID(dev string) (string, error)        // major:minor
	DMName(dev string) (string, error)    // /sys/class/block/<dev>/dm/name
}
type releasePlan struct {
	Unmount []string // mount points, deepest first
	Remove  []string // dm names, top of stack first
}

func (p releasePlan) empty() bool { return len(p.Unmount) == 0 && len(p.Remove) == 0 }
func (p releasePlan) describe() []string {
	var out []string
	for _, m := range p.Unmount {
		out = append(out, "unmount "+m)
	}
	for _, d := range p.Remove {
		out = append(out, "release mapping "+d)
	}
	return out
}

// unescape mountinfo octal escapes (\040 space etc.).
func unescape(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+3 < len(s) {
			if v, e := strconv.ParseUint(s[i+1:i+4], 8, 8); e == nil {
				b.WriteByte(byte(v))
				i += 3
				continue
			}
		}
		b.WriteByte(s[i])
	}
	return b.String()
}

// planRelease: targets maps kernel names (sda24) to partition labels.
func planRelease(targets map[string]string, mountinfo string, v blockView, archiveID string) (releasePlan, error) {
	var plan releasePlan
	ids := map[string]string{} // major:minor -> kernel name for targets and their dm stack
	depth := map[string]int{}
	var walk func(dev string, d int) error
	walk = func(dev string, d int) error {
		if d > 8 {
			return errors.New("device-mapper stack too deep")
		}
		id, e := v.ID(dev)
		if e != nil {
			return e
		}
		if old, ok := depth[dev]; ok && old >= d {
			return nil
		}
		ids[id], depth[dev] = dev, d
		holders, e := v.Holders(dev)
		if e != nil {
			return e
		}
		for _, h := range holders {
			if !strings.HasPrefix(h, "dm-") {
				return fmt.Errorf("%s is held by %s (not device-mapper); refusing", dev, h)
			}
			if e = walk(h, d+1); e != nil {
				return e
			}
		}
		return nil
	}
	for dev := range targets {
		if e := walk(dev, 0); e != nil {
			return plan, e
		}
	}
	// Every dm device we would remove must sit only on targets or other planned dm.
	var dms []string
	for dev, d := range depth {
		if d == 0 {
			continue
		}
		slaves, e := v.Slaves(dev)
		if e != nil {
			return plan, e
		}
		for _, s := range slaves {
			if _, ok := depth[s]; !ok {
				return plan, fmt.Errorf("mapping %s also uses %s (not an installer target); refusing", dev, s)
			}
		}
		dms = append(dms, dev)
	}
	if archiveID != "" {
		if dev, ok := ids[archiveID]; ok {
			return plan, fmt.Errorf("the ZIP is stored on %s, which installation replaces; use microSD, USB-OTG or sideload", dev)
		}
	}
	sort.Slice(dms, func(i, j int) bool {
		if depth[dms[i]] != depth[dms[j]] {
			return depth[dms[i]] > depth[dms[j]]
		}
		return dms[i] < dms[j]
	})
	for _, dev := range dms {
		name, e := v.DMName(dev)
		if e != nil || name == "" || strings.ContainsAny(name, "/ \n") {
			return plan, fmt.Errorf("cannot name mapping %s", dev)
		}
		plan.Remove = append(plan.Remove, name)
	}
	lines := strings.Split(strings.TrimSpace(mountinfo), "\n")
	for _, line := range lines {
		f := strings.Fields(line)
		if len(f) < 5 {
			continue
		}
		if _, ok := ids[f[2]]; ok {
			plan.Unmount = append(plan.Unmount, unescape(f[4]))
		}
	}
	sort.Slice(plan.Unmount, func(i, j int) bool {
		a, b := strings.Count(plan.Unmount[i], "/"), strings.Count(plan.Unmount[j], "/")
		if a != b {
			return a > b
		}
		return plan.Unmount[i] > plan.Unmount[j]
	})
	return plan, nil
}
