// SPDX-License-Identifier: MIT
package main

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
)

func mountedDevices(text string) (map[string]bool, error) {
	mounted := make(map[string]bool)
	lines := strings.Split(strings.TrimSpace(text), "\n")
	for _, line := range lines {
		fields := strings.Fields(line)
		if len(fields) < 10 || !strings.Contains(line, " - ") {
			return nil, errors.New("malformed/missing mount evidence")
		}
		id := strings.Split(fields[2], ":")
		if len(id) != 2 {
			return nil, errors.New("malformed mount device identity")
		}
		for _, part := range id {
			if _, e := strconv.ParseUint(part, 10, 32); e != nil {
				return nil, errors.New("malformed mount device identity")
			}
		}
		mounted[fields[2]] = true
	}
	return mounted, nil
}
func verifyIdentity(c Config, model, bootloader, product, soc, locked, state string) error {
	if !hasModel(c.Models, model) || !hasModel(c.Bootloaders, bootloader) {
		return fmt.Errorf("exact model/bootloader mismatch: model=%q bootloader=%q", model, bootloader)
	}
	// The product only names the TWRP build. Unified Exynos 990 builds (ExtremeROM/Artisan) report one
	// family member (z3s) on every phone; the exact model and bootloader above are the real identity.
	if !hasModel(c.Products, product) && !hasModel(exynos990TWRPProducts, product) {
		return fmt.Errorf("unqualified recovery product: %s", product)
	}
	if soc != "exynos990" && soc != "exynos9830" {
		return fmt.Errorf("unqualified SoC property: %s", soc)
	}
	if (locked != "0" && state != "unlocked") || (locked != "" && locked != "0") || (state != "" && state != "unlocked") {
		return errors.New("unlocked bootloader evidence missing/conflicting")
	}
	return nil
}

// Samsung recovery templates can report a generic product model. Prefer the
// bootloader-supplied hardware model used by ExtremeROM's device assertions.
func verifyRecoveryIdentity(c Config, get func(string) string) error {
	model := get("ro.boot.em.model")
	if model == "" {
		model = get("ro.product.model")
	}
	bootloader, fallback := get("ro.bootloader"), get("ro.boot.bootloader")
	if bootloader != "" && fallback != "" && bootloader != fallback {
		return errors.New("conflicting bootloader properties")
	}
	if bootloader == "" {
		bootloader = fallback
	}
	soc := get("ro.board.platform")
	if soc == "" {
		soc = get("ro.hardware")
	}
	locked, state := get("ro.boot.flash.locked"), get("ro.boot.vbmeta.device_state")
	// Some Samsung TWRP builds report neither; AVB's "orange" boot state is the bootloader's own word for unlocked.
	if locked == "" && state == "" && get("ro.boot.verifiedbootstate") == "orange" {
		state = "unlocked"
	}
	return verifyIdentity(c, model, bootloader, get("ro.product.device"), soc, locked, state)
}

// TWRP product names of the Exynos 990 family, accepted only after the exact model/bootloader match.
var exynos990TWRPProducts = []string{"x1s", "x1slte", "y2s", "y2slte", "z3s", "z3slte", "c1s", "c2s", "r8s"}
