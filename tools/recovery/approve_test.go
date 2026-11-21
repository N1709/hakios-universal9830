// SPDX-License-Identifier: MIT
package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestComputerApproval(t *testing.T) {
	approvalFile = filepath.Join(t.TempDir(), "approve")
	configSHA = "abc"
	defer func() { configSHA = "" }()
	nolog := func(string) {}
	cases := []struct {
		file   string // "" = no file
		erases bool
		ok     bool
	}{
		{"abc keep", false, true},
		{"abc erase", false, true},
		{"abc erase", true, true},
		{"abc keep", true, false},    // an update approval never covers an erase
		{"other keep", false, false}, // another package's approval: keys (none on the host)
		{"abc maybe", false, false},
		{"", false, false},
	}
	for _, c := range cases {
		os.Remove(approvalFile)
		if c.file != "" {
			os.WriteFile(approvalFile, []byte(c.file+"\n"), 0600)
		}
		if e := confirm("x", c.erases, nolog); (e == nil) != c.ok {
			t.Fatalf("%q erases=%v: %v", c.file, c.erases, e)
		}
	}
	dropApproval()
	if _, e := os.Stat(approvalFile); !os.IsNotExist(e) {
		t.Fatal("approval not removed")
	}
}
