// SPDX-License-Identifier: MIT
package main

import (
	"errors"
	"reflect"
	"strings"
	"testing"
)

type fakeBlocks struct {
	holders, slaves map[string][]string
	ids, names      map[string]string
}

func (f fakeBlocks) Holders(d string) ([]string, error) { return f.holders[d], nil }
func (f fakeBlocks) Slaves(d string) ([]string, error)  { return f.slaves[d], nil }
func (f fakeBlocks) ID(d string) (string, error) {
	if id, ok := f.ids[d]; ok {
		return id, nil
	}
	return "", errors.New("no such device")
}
func (f fakeBlocks) DMName(d string) (string, error) { return f.names[d], nil }

// TWRP booted on ExtremeROM: super mapped as system/vendor/product/odm, /data on
// dm-default-key over userdata, MTP storage bind-mounted below /data.
func extremeTWRP() (fakeBlocks, string) {
	f := fakeBlocks{
		holders: map[string][]string{"sda32": {"dm-0", "dm-1", "dm-2", "dm-3"}, "sda33": {"dm-4"}},
		slaves:  map[string][]string{"dm-0": {"sda32"}, "dm-1": {"sda32"}, "dm-2": {"sda32"}, "dm-3": {"sda32"}, "dm-4": {"sda33"}},
		ids:     map[string]string{"sda32": "259:16", "sda33": "259:17", "dm-0": "253:0", "dm-1": "253:1", "dm-2": "253:2", "dm-3": "253:3", "dm-4": "253:4", "mmcblk0p1": "179:1"},
		names:   map[string]string{"dm-0": "system", "dm-1": "vendor", "dm-2": "product", "dm-3": "odm", "dm-4": "userdata"},
	}
	mounts := strings.Join([]string{
		"20 1 0:2 / / rw - rootfs rootfs rw",
		"30 20 253:0 / /system_root ro - ext4 /dev/block/dm-0 ro",
		"31 20 253:1 / /vendor ro - ext4 /dev/block/dm-1 ro",
		"32 20 253:4 / /data rw - f2fs /dev/block/dm-4 rw",
		"33 32 253:4 /media /data/media/0\\040storage rw - f2fs /dev/block/dm-4 rw",
		"34 20 179:1 / /external_sd rw - vfat /dev/block/mmcblk0p1 rw",
	}, "\n")
	return f, mounts
}

func TestAndroidROMHoldsArePlannedDeepestFirst(t *testing.T) {
	f, mounts := extremeTWRP()
	p, e := planRelease(map[string]string{"sda32": "super", "sda33": "userdata"}, mounts, f, "179:1")
	if e != nil {
		t.Fatal(e)
	}
	if !reflect.DeepEqual(p.Unmount, []string{"/data/media/0 storage", "/vendor", "/system_root", "/data"}) {
		t.Fatal(p.Unmount)
	}
	if !reflect.DeepEqual(p.Remove, []string{"system", "vendor", "product", "odm", "userdata"}) {
		t.Fatal(p.Remove)
	}
}

func TestReleaseRefusesForeignStacksAndZipOnTarget(t *testing.T) {
	f, mounts := extremeTWRP()
	f.slaves["dm-4"] = []string{"sda33", "sda5"}
	if _, e := planRelease(map[string]string{"sda32": "super", "sda33": "userdata"}, mounts, f, ""); e == nil {
		t.Fatal("mapping spanning a non-target device accepted")
	}
	f, mounts = extremeTWRP()
	if _, e := planRelease(map[string]string{"sda32": "super", "sda33": "userdata"}, mounts, f, "253:4"); e == nil {
		t.Fatal("ZIP on /data accepted")
	}
	f.holders["sda32"] = []string{"loop0"}
	if _, e := planRelease(map[string]string{"sda32": "super"}, mounts, f, ""); e == nil {
		t.Fatal("non-dm holder accepted")
	}
}

func TestIOSPhoneNeedsNoRelease(t *testing.T) {
	f := fakeBlocks{ids: map[string]string{"sda32": "259:16", "sda33": "259:17"}}
	p, e := planRelease(map[string]string{"sda32": "super", "sda33": "userdata"}, "20 1 0:2 / / rw - rootfs rootfs rw", f, "179:1")
	if e != nil || !p.empty() {
		t.Fatal(p, e)
	}
}
