// SPDX-License-Identifier: MIT
package main

import (
	"bytes"
	"strings"
	"testing"
)

func TestScreenFitsAndReaches100(t *testing.T) {
	var out bytes.Buffer
	s := &screen{pipe: &out}
	s.header("iOS 16.5 for the Galaxy S20", "v0.1.1 (2026-11-19)", "SM-G980F")
	s.steps = 3
	s.begin("Checking the package")
	s.plan(3, 1000)
	s.begin("Installing the iOS 16.5 system")
	for i := 0; i < 100; i++ {
		s.advance(10)
	}
	s.finish("Jesvi iOS is installed")
	text := out.String()
	for _, l := range strings.Split(text, "\n") {
		if strings.HasPrefix(l, "ui_print ") && len(l)-len("ui_print ") > 40 {
			t.Errorf("line wider than TWRP's console: %q", l)
		}
	}
	if !strings.Contains(text, "100%") || !strings.Contains(text, "set_progress 1.000") {
		t.Fatalf("bar never reached 100 %%:\n%s", text)
	}
	if testing.Verbose() {
		for _, l := range strings.Split(text, "\n") {
			if strings.HasPrefix(l, "ui_print") {
				t.Log(strings.TrimPrefix(strings.TrimPrefix(l, "ui_print"), " "))
			}
		}
	}
}
