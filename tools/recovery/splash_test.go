// SPDX-License-Identifier: MIT
package main

import (
	"archive/tar"
	"bytes"
	"os"
	"testing"
)

func pictures(t *testing.T, size int, files ...[2]string) []byte {
	t.Helper()
	var buf bytes.Buffer
	w := tar.NewWriter(&buf)
	for _, f := range files {
		h := &tar.Header{Name: f[0], Mode: 0444, Size: int64(len(f[1])), Uname: "dpi", Gname: "dpi", Format: tar.FormatUSTAR}
		if e := w.WriteHeader(h); e != nil {
			t.Fatal(e)
		}
		w.Write([]byte(f[1]))
	}
	w.Close()
	return append(buf.Bytes(), make([]byte, size-buf.Len())...)
}

var jpeg = []byte("\xff\xd8apple splash\xff\xd9")

func members(t *testing.T, img []byte) map[string]string {
	t.Helper()
	ms, e := parsePictures(img)
	if e != nil {
		t.Fatal(e)
	}
	out := map[string]string{}
	for _, m := range ms {
		out[m.Header.Name] = string(m.Data)
	}
	return out
}

func TestSplashRoundTrip(t *testing.T) {
	stock := pictures(t, 1<<20, [2]string{"download.jpg", "dl"}, [2]string{"logo.jpg", "samsung"}, [2]string{"warning.jpg", "w"})
	img, changed, e := withSplash(stock, jpeg)
	if e != nil || !changed || len(img) != len(stock) {
		t.Fatalf("withSplash: %v %v %d", e, changed, len(img))
	}
	m := members(t, img)
	if m["logo.jpg"] != string(jpeg) || m["logo.jpg.stock"] != "samsung" || m["download.jpg"] != "dl" || m["warning.jpg"] != "w" || len(m) != 4 {
		t.Fatalf("after install: %v", m)
	}
	// installing again keeps the phone's original, not our splash, as the saved copy
	again, changed, e := withSplash(img, jpeg)
	if e != nil || changed || !bytes.Equal(again, img) {
		t.Fatal("second install must be a no-op")
	}
	other := []byte("\xff\xd8newer splash")
	upd, changed, e := withSplash(img, other)
	if e != nil || !changed || members(t, upd)["logo.jpg.stock"] != "samsung" {
		t.Fatal("an update must keep the first saved logo")
	}
	back, changed, e := withoutSplash(upd)
	if e != nil || !changed {
		t.Fatal(e)
	}
	m = members(t, back)
	if m["logo.jpg"] != "samsung" || len(m) != 3 {
		t.Fatalf("after restore: %v", m)
	}
	if _, changed, _ = withoutSplash(back); changed {
		t.Fatal("restore of a stock up_param must be a no-op")
	}
}

func TestSplashRefusals(t *testing.T) {
	stock := pictures(t, 1<<16, [2]string{"logo.jpg", "samsung"})
	if _, _, e := withSplash(stock, []byte("not a jpeg")); e == nil {
		t.Fatal("non-JPEG accepted")
	}
	if _, _, e := withSplash(pictures(t, 1<<16, [2]string{"download.jpg", "dl"}), jpeg); e == nil {
		t.Fatal("up_param without logo.jpg accepted")
	}
	if _, _, e := withSplash(make([]byte, 1<<16), jpeg); e == nil {
		t.Fatal("empty partition accepted")
	}
	big := make([]byte, 70<<10)
	copy(big, jpeg)
	if _, _, e := withSplash(pictures(t, 1<<16, [2]string{"logo.jpg", "samsung"}), big); e == nil {
		t.Fatal("splash larger than up_param accepted")
	}
	var buf bytes.Buffer
	w := tar.NewWriter(&buf)
	w.WriteHeader(&tar.Header{Name: "../evil", Size: 1, Mode: 0444})
	w.Write([]byte("x"))
	w.Close()
	if _, _, e := withSplash(append(buf.Bytes(), make([]byte, 1<<16)...), jpeg); e == nil {
		t.Fatal("path member accepted")
	}
}

// JESVI_UP_PARAM=stock-up_param.img JESVI_SPLASH=logo.jpg go test -run Real: the phone's real pictures (never committed)
func TestSplashRealUpParam(t *testing.T) {
	p, s := os.Getenv("JESVI_UP_PARAM"), os.Getenv("JESVI_SPLASH")
	if p == "" || s == "" {
		t.Skip("set JESVI_UP_PARAM and JESVI_SPLASH")
	}
	stock, e1 := os.ReadFile(p)
	splash, e2 := os.ReadFile(s)
	if e1 != nil || e2 != nil {
		t.Fatal(e1, e2)
	}
	before := members(t, stock)
	img, changed, e := withSplash(stock, splash)
	if e != nil || !changed {
		t.Fatal(e)
	}
	after := members(t, img)
	for n, d := range before {
		if n == "logo.jpg" {
			if after["logo.jpg.stock"] != d {
				t.Fatal("stock logo not kept")
			}
			continue
		}
		if after[n] != d {
			t.Fatalf("%s changed", n)
		}
	}
	back, _, e := withoutSplash(img)
	if e != nil {
		t.Fatal(e)
	}
	restored := members(t, back)
	for n, d := range before {
		if restored[n] != d {
			t.Fatalf("%s not restored", n)
		}
	}
	if len(restored) != len(before) {
		t.Fatal("restore left extra members")
	}
	t.Logf("%d pictures, splash %d bytes, %d bytes of tar used", len(before), len(splash), len(bytes.TrimRight(img, "\x00")))
}

// The whole installer: install sets the splash last (after boot) and keeps the phone's own; restore puts it back.
func TestInstallAndRestoreSplash(t *testing.T) {
	capacities["up_param"] = 1 << 20
	t.Cleanup(func() { delete(capacities, "up_param") })
	b, d := fixtureBundle(t), newFake(t)
	stock := pictures(t, 1<<20, [2]string{"download.jpg", "dl"}, [2]string{"logo.jpg", "samsung"})
	d.files["up_param"].WriteAt(stock, 0)
	b.Splash = jpeg
	var log []string
	if e := perform(b, d, "fresh", func(s string) { log = append(log, s) }); e != nil {
		t.Fatal(e)
	}
	if d.opened[len(d.opened)-1] != "up_param" {
		t.Fatalf("up_param must be opened with the other targets: %v", d.opened)
	}
	m := members(t, readBytes(t, d.files["up_param"], 0, 1<<20))
	if m["logo.jpg"] != string(jpeg) || m["logo.jpg.stock"] != "samsung" || m["download.jpg"] != "dl" {
		t.Fatalf("installed pictures: %v", m)
	}
	for i, s := range log {
		if s == "Setting iPhone's power-on and charging pictures (the phone's own kept for Jesvi-iOS-Restore)" {
			for _, later := range log[:i] {
				if later == "Writing and verifying boot" {
					goto ordered
				}
			}
			t.Fatalf("splash before boot: %v", log)
		}
	}
	t.Fatalf("no splash step: %v", log)
ordered:
	nx := make([]byte, 64)
	copy(nx[32:], "NXSB")
	d.files["super"].WriteAt(nx, 0)
	r := &Bundle{Config: testConfig()}
	r.Config.Action, r.Config.Mode = "restore", "restore"
	if e := restoreAndroid(r, d, func(string) {}); e != nil {
		t.Fatal(e)
	}
	m = members(t, readBytes(t, d.files["up_param"], 0, 1<<20))
	if m["logo.jpg"] != "samsung" || len(m) != 2 {
		t.Fatalf("restored pictures: %v", m)
	}
}

// The charging pictures go with the logo, each kept as .stock; one the phone lacks is skipped; Restore puts all back.
func TestChargingPictures(t *testing.T) {
	stock := pictures(t, 1<<20, [2]string{"logo.jpg", "samsung"}, [2]string{"lpm.jpg", "bolt"}, [2]string{"download.jpg", "dl"})
	lpm := append([]byte{0xff, 0xd8}, []byte("iphone charging")...)
	low := append([]byte{0xff, 0xd8}, []byte("iphone empty")...)
	img, changed, e := withPictures(stock, jpeg, map[string][]byte{"lpm.jpg": lpm, "low_battery_alert.jpg": low})
	if e != nil || !changed {
		t.Fatal(e, changed)
	}
	m := members(t, img)
	if m["lpm.jpg"] != string(lpm) || m["lpm.jpg.stock"] != "bolt" || m["logo.jpg.stock"] != "samsung" || m["low_battery_alert.jpg"] != "" {
		t.Fatalf("installed: %v", m)
	}
	if _, _, e := withPictures(stock, jpeg, map[string][]byte{"download.jpg": lpm}); e == nil {
		t.Fatal("only the charging pictures may be replaced")
	}
	back, changed, e := withoutSplash(img)
	m = members(t, back)
	if e != nil || !changed || m["lpm.jpg"] != "bolt" || m["logo.jpg"] != "samsung" || m["lpm.jpg.stock"] != "" || m["logo.jpg.stock"] != "" {
		t.Fatalf("restored: %v %v", m, e)
	}
}
