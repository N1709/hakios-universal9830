// SPDX-License-Identifier: MIT
package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"hash/crc32"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func testKit() []byte {
	size := dataBytes
	primary := make([]byte, 4096)
	backup := make([]byte, 4096)
	header := func(h []byte) {
		copy(h, "HX")
		binary.BigEndian.PutUint16(h[2:4], 5)
		binary.BigEndian.PutUint32(h[40:44], 4096)
		binary.BigEndian.PutUint32(h[44:48], uint32(size/4096))
	}
	header(primary[1024:])
	header(backup[3072:])
	body := make([]byte, 48)
	binary.LittleEndian.PutUint32(body[8:12], 1)
	binary.LittleEndian.PutUint32(body[12:16], 1)
	binary.LittleEndian.PutUint64(body[16:24], 2)
	binary.LittleEndian.PutUint32(body[24:28], 1) // zero extent
	binary.LittleEndian.PutUint64(body[32:40], uint64(size/4096-1))
	binary.LittleEndian.PutUint32(body[40:44], 1)
	binary.LittleEndian.PutUint32(body[44:48], 1)
	body = append(body, primary...)
	body = append(body, backup...)
	kit := make([]byte, 32)
	copy(kit, "HFSKIT01")
	binary.LittleEndian.PutUint32(kit[8:12], 4096)
	binary.LittleEndian.PutUint32(kit[12:16], 3)
	binary.LittleEndian.PutUint64(kit[16:24], uint64(size))
	binary.LittleEndian.PutUint32(kit[24:28], crc32.ChecksumIEEE(body))
	return append(kit, body...)
}
func fixtureBundle(t *testing.T) *Bundle {
	t.Helper()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	c := testConfig()
	c.Files = make(map[string]Image)
	for name, part := range imageParts {
		data := bytes.Repeat([]byte(name), 500)
		h := sha256.Sum256(data)
		c.Files[name] = Image{Partition: part, Bytes: int64(len(data)), SHA256: hex.EncodeToString(h[:])}
		w, e := zw.CreateHeader(&zip.FileHeader{Name: name, Method: zip.Store})
		if e != nil {
			t.Fatal(e)
		}
		w.Write(data)
	}
	if e := zw.Close(); e != nil {
		t.Fatal(e)
	}
	zr, e := zip.NewReader(bytes.NewReader(buf.Bytes()), int64(buf.Len()))
	if e != nil {
		t.Fatal(e)
	}
	files := make(map[string]*zip.File)
	for _, f := range zr.File {
		files[f.Name] = f
	}
	return &Bundle{Config: c, Files: files, Kit: testKit()}
}

type fakeDevice struct {
	files             map[string]*os.File
	opened            []string
	unsafe            bool
	failOpen, corrupt string
}

func newFake(t *testing.T) *fakeDevice {
	t.Helper()
	d := &fakeDevice{files: make(map[string]*os.File)}
	dir := t.TempDir()
	for name, size := range capacities {
		f, e := os.OpenFile(filepath.Join(dir, name), os.O_CREATE|os.O_RDWR, 0600)
		if e != nil {
			t.Fatal(e)
		}
		if e = f.Truncate(size); e != nil {
			t.Fatal(e)
		}
		d.files[name] = f
		f.WriteAt([]byte("UNCHANGED-TAIL"), size-64)
		t.Cleanup(func() { f.Close() })
	}
	data := testKit()[32+48 : 32+48+4096]
	d.files["userdata"].WriteAt(data, 0)
	d.files["userdata"].WriteAt([]byte("PRIVATE-KEYBAG-CANARY"), 16000)
	return d
}
func (d *fakeDevice) CheckSafe() error {
	if d.unsafe {
		return errors.New("mounted")
	}
	return nil
}
func (d *fakeDevice) CheckData() error {
	if !initialized(d.files["userdata"], capacities["userdata"]) {
		return errors.New("bad HFS")
	}
	return nil
}
func (d *fakeDevice) Close() error { return nil }

type badTarget struct{ *os.File }

func (b badTarget) ReadAt(p []byte, off int64) (int, error) {
	n, e := b.File.ReadAt(p, off)
	if n > 0 {
		p[0] ^= 1
	}
	return n, e
}
func (d *fakeDevice) OpenWritable(name string) (Target, error) {
	d.opened = append(d.opened, name)
	if name == d.failOpen {
		return nil, errors.New("busy")
	}
	f := d.files[name]
	if f == nil {
		return nil, errors.New("unknown partition")
	}
	if name == d.corrupt {
		return badTarget{f}, nil
	}
	return f, nil
}
func readBytes(t *testing.T, f *os.File, off int64, n int) []byte {
	t.Helper()
	p := make([]byte, n)
	if _, e := f.ReadAt(p, off); e != nil {
		t.Fatal(e)
	}
	return p
}
func TestUpdatePreservesDataAndTailsAndCommitsBootLast(t *testing.T) {
	b, d := fixtureBundle(t), newFake(t)
	secret := readBytes(t, d.files["userdata"], 16000, 21)
	var log []string
	if e := perform(b, d, "update", func(s string) { log = append(log, s) }); e != nil {
		t.Fatal(e)
	}
	for name, m := range b.Config.Files {
		r, _ := b.Files[name].Open()
		want, _ := io.ReadAll(r)
		r.Close()
		if !bytes.Equal(want, readBytes(t, d.files[m.Partition], 0, int(m.Bytes))) {
			t.Fatal(name)
		}
	}
	if !bytes.Equal(secret, readBytes(t, d.files["userdata"], 16000, 21)) {
		t.Fatal("update changed keybag canary")
	}
	for name, n := range capacities {
		if string(readBytes(t, d.files[name], n-64, 14)) != "UNCHANGED-TAIL" {
			t.Fatal("tail changed", name)
		}
	}
	if !reflect.DeepEqual(d.opened, []string{"super", "recovery", "boot"}) {
		t.Fatal(d.opened)
	}
	want := []string{"No Android partition map at the start of super (already iOS); keeping any saved copy", "Writing and verifying super", "Writing and verifying recovery", "Writing and verifying boot", "All written ranges passed readback. No reboot requested."}
	if !reflect.DeepEqual(log, want) {
		t.Fatal(log)
	}
}
func TestFreshInitializesHFSHeadersAndZeroExtents(t *testing.T) {
	b, d := fixtureBundle(t), newFake(t)
	d.files["userdata"].WriteAt(bytes.Repeat([]byte{255}, 4096), 8192)
	if e := perform(b, d, "fresh", func(string) {}); e != nil {
		t.Fatal(e)
	}
	if !initialized(d.files["userdata"], capacities["userdata"]) {
		t.Fatal("primary missing")
	}
	if !validHFS(readBytes(t, d.files["userdata"], capacities["userdata"]-1024, 512), capacities["userdata"]) {
		t.Fatal("alternate missing")
	}
	if !bytes.Equal(readBytes(t, d.files["userdata"], 8192, 4096), make([]byte, 4096)) {
		t.Fatal("zero extent not initialized")
	}
}
func TestFailuresStopBeforeBootAndNeverFormatOnUpdateFailure(t *testing.T) {
	for _, which := range []string{"mounted", "busy", "readback", "bad-data", "bad-kit"} {
		t.Run(which, func(t *testing.T) {
			b, d := fixtureBundle(t), newFake(t)
			if which == "mounted" {
				d.unsafe = true
			}
			if which == "busy" {
				d.failOpen = "recovery"
			}
			if which == "readback" {
				d.corrupt = "super"
			}
			if which == "bad-data" {
				d.files["userdata"].WriteAt([]byte("BAD!"), 1024)
			}
			if which == "bad-kit" {
				b.Kit[len(b.Kit)-1] ^= 1
			}
			if e := perform(b, d, "update", func(string) {}); e == nil {
				t.Fatal("expected failure")
			}
			if !bytes.Equal(readBytes(t, d.files["boot"], 0, 4096), make([]byte, 4096)) {
				t.Fatal("BOOT modified after failure")
			}
			if !bytes.Equal(readBytes(t, d.files["recovery"], 0, 4096), make([]byte, 4096)) {
				t.Fatal("recovery modified after failure")
			}
			if string(readBytes(t, d.files["userdata"], 16000, 21)) != "PRIVATE-KEYBAG-CANARY" {
				t.Fatal("userdata changed on failed update")
			}
		})
	}
}
func TestShortWriteAndSourceMismatchRefuseSuccess(t *testing.T) {
	d := newFake(t)
	h := sha256.Sum256([]byte("abcd"))
	if e := transfer(d.files["super"], bytes.NewReader([]byte("ab")), 0, 4, hex.EncodeToString(h[:])); e == nil {
		t.Fatal("short source accepted")
	}
	if e := transfer(d.files["super"], bytes.NewReader([]byte("xxxx")), 0, 4, hex.EncodeToString(h[:])); e == nil {
		t.Fatal("changed source accepted")
	}
}
func TestKitRejectsOverlapBadCRCAndWrongCapacity(t *testing.T) {
	kit := testKit()
	if _, e := parseKit(kit, capacities["userdata"]); e != nil {
		t.Fatal(e)
	}
	if _, e := parseKit(kit, 4096); e == nil {
		t.Fatal("wrong capacity accepted")
	}
	kit[32+16] = 0
	binary.LittleEndian.PutUint32(kit[24:28], crc32.ChecksumIEEE(kit[32:]))
	if _, e := parseKit(kit, capacities["userdata"]); e == nil {
		t.Fatal("overlap accepted")
	}
	kit = testKit()
	kit[len(kit)-1] ^= 1
	if _, e := parseKit(kit, capacities["userdata"]); e == nil {
		t.Fatal("CRC failure accepted")
	}
}

// A second pass must independently verify installed bytes before skipping writes.
func TestIdenticalInstalledImagesAreVerifiedAndSkipped(t *testing.T) {
	b, d := fixtureBundle(t), newFake(t)
	if e := perform(b, d, "update", func(string) {}); e != nil {
		t.Fatal(e)
	}
	var logs []string
	if e := perform(b, d, "update", func(s string) { logs = append(logs, s) }); e != nil {
		t.Fatal(e)
	}
	want := []string{"No Android partition map at the start of super (already iOS); keeping any saved copy", "Already matches verified super; skipping write", "Already matches verified recovery; skipping write", "Already matches verified boot; skipping write", "All written ranges passed readback. No reboot requested."}
	if !reflect.DeepEqual(logs, want) {
		t.Fatal(logs)
	}
}

func TestRecoveryAndFreshDataFailureNeverCommitBoot(t *testing.T) {
	for _, part := range []string{"recovery", "userdata"} {
		t.Run(part, func(t *testing.T) {
			b, d := fixtureBundle(t), newFake(t)
			d.corrupt = part
			mode := "update"
			if part == "userdata" {
				mode = "fresh"
			}
			if e := perform(b, d, mode, func(string) {}); e == nil {
				t.Fatal("expected readback failure")
			}
			if !bytes.Equal(readBytes(t, d.files["boot"], 0, 4096), make([]byte, 4096)) {
				t.Fatal("BOOT committed after prior failure")
			}
			if part == "userdata" && !bytes.Equal(readBytes(t, d.files["recovery"], 0, 4096), make([]byte, 4096)) {
				t.Fatal("recovery changed after userdata failure")
			}
		})
	}
}

func TestStagedBootAndRecoveryDoNotDependOnTransportDuringCommit(t *testing.T) {
	b, d := fixtureBundle(t), newFake(t)
	b.Staged = make(map[string][]byte)
	for _, name := range []string{"boot.img", "recovery.img"} {
		r, _ := b.Files[name].Open()
		data, _ := io.ReadAll(r)
		r.Close()
		b.Staged[name] = data
		delete(b.Files, name)
	}
	if e := perform(b, d, "update", func(string) {}); e != nil {
		t.Fatal(e)
	}
}

// Android phone (ExtremeROM etc.): its partition map is saved in super's tail
// before iOS overwrites the head, and the restore ZIP brings back an empty map.
func TestInstallSavesAndroidMapAndRestoreBringsItBack(t *testing.T) {
	b, d := fixtureBundle(t), newFake(t)
	m := s20Android().metadata(capacities["super"])
	m.Groups[1].Name = "rom_group" // as a custom ROM might name it
	m.Partitions = []lpPartition{{Name: "system", Attributes: 1, NumExtent: 1, Group: 1}}
	m.Extents = []lpExtent{{Sectors: 2048, Data: 2048}}
	android, e := m.encode()
	if e != nil {
		t.Fatal(e)
	}
	d.files["super"].WriteAt(android, 0)
	var log []string
	if e = perform(b, d, "fresh", func(s string) { log = append(log, s) }); e != nil {
		t.Fatal(e)
	}
	if log[0] != "Saving the Android partition map for Jesvi-iOS-Restore" {
		t.Fatal(log)
	}
	if string(readBytes(t, d.files["super"], capacities["super"]-64, 14)) != "UNCHANGED-TAIL" {
		t.Fatal("stash overran super's tail")
	}
	// The test system image is not APFS; give super an iOS head as a real install has.
	nx := make([]byte, 64)
	copy(nx[32:], "NXSB")
	d.files["super"].WriteAt(nx, 0)
	r := &Bundle{Config: testConfig()}
	r.Config.Action, r.Config.Mode = "restore", "restore"
	log = nil
	if e = restoreAndroid(r, d, func(s string) { log = append(log, s) }); e != nil {
		t.Fatal(e)
	}
	got, e := parseLP(readBytes(t, d.files["super"], 0, lpRegion))
	if e != nil || len(got.Partitions) != 4 || got.Partitions[0].Name != "system" || got.Groups[1].Name != "rom_group" { // placeholders for TWRP
		t.Fatal("restored map wrong", e, log)
	}
	if h := readBytes(t, d.files["userdata"], 1024, 2); string(h) == "HX" {
		t.Fatal("HFSX header survived restore")
	}
	if !bytes.Equal(readBytes(t, d.files["userdata"], capacities["userdata"]-1024, 512), make([]byte, 512)) {
		t.Fatal("HFSX alternate header survived restore")
	}
	// Second run is a no-op; an Android super with partitions is never touched.
	log = nil
	if e = restoreAndroid(r, d, func(s string) { log = append(log, s) }); e != nil || len(log) != 2 {
		t.Fatal(e, log)
	}
	d.files["super"].WriteAt(android, 0)
	if e = restoreAndroid(r, d, func(string) {}); e == nil {
		t.Fatal("restore overwrote a live Android partition map")
	}
}

func TestRestoreWithoutSavedMapUsesMeasuredLayoutAndRefusesUnknownSuper(t *testing.T) {
	d := newFake(t)
	r := &Bundle{Config: testConfig()}
	r.Config.Action, r.Config.Mode = "restore", "restore"
	if e := restoreAndroid(r, d, func(string) {}); e == nil {
		t.Fatal("restore accepted a super that is neither iOS nor Android")
	}
	nx := make([]byte, 64)
	copy(nx[32:], "NXSB")
	d.files["super"].WriteAt(nx, 0)
	var log []string
	if e := restoreAndroid(r, d, func(s string) { log = append(log, s) }); e != nil {
		t.Fatal(e)
	}
	got, e := parseLP(readBytes(t, d.files["super"], 0, lpRegion))
	if e != nil || got.Groups[1].Name != "group_basic" || got.Devices[0].Alignment != 524288 {
		t.Fatal(e, log)
	}
	d.unsafe = true
	copy(nx[32:], "NXSB")
	d.files["super"].WriteAt(nx, 0)
	if e := restoreAndroid(r, d, func(string) {}); e == nil {
		t.Fatal("restore wrote while targets were mounted")
	}
}

// IOS_DATA_GB=64: the volume ends at 64 GB and nothing after it (left for another OS) is written.
func TestFreshInstallInsideA64GBWindow(t *testing.T) {
	saved := dataBytes
	dataBytes = 64e9
	defer func() { dataBytes = saved }()
	b, d := fixtureBundle(t), newFake(t)
	mark := bytes.Repeat([]byte("ANDROID!"), 512)
	d.files["userdata"].WriteAt(mark, 64e9+4096)
	d.files["userdata"].WriteAt(mark, capacities["userdata"]-4096)
	if e := perform(b, d, "fresh", func(string) {}); e != nil {
		t.Fatal(e)
	}
	if !initialized(d.files["userdata"], dataBytes) || !validHFS(readBytes(t, d.files["userdata"], 64e9-1024, 512), dataBytes) {
		t.Fatal("64 GB volume headers missing")
	}
	if !bytes.Equal(readBytes(t, d.files["userdata"], 64e9+4096, len(mark)), mark) ||
		!bytes.Equal(readBytes(t, d.files["userdata"], capacities["userdata"]-4096, len(mark)), mark) {
		t.Fatal("wrote past the iOS data window")
	}
}
