// SPDX-License-Identifier: MIT
package main

// Samsung's power-on splash (A, 2026-11-15). up_param is a tar of the bootloader's JPEG pictures; logo.jpg is the
// one shown at power-on. The installer replaces only that member with the release's splash (iOS's boot logo, made at
// build time from the user's own iOS root) and keeps the phone's original as logo.jpg.stock in the same tar, so
// every other picture of that phone's bootloader version stays byte for byte, and Jesvi-iOS-Restore can put the
// original back without any other copy. Nothing here is written unless the whole new tar parses back to exactly
// the intended members.

import (
	"archive/tar"
	"bytes"
	"errors"
	"fmt"
	"io"
	"strings"
)

const (
	splashName  = "logo.jpg"
	splashStock = "logo.jpg.stock"
	stockSuffix = ".stock"
	splashMax   = 1 << 20
)

// extraPictures: the other pictures S-Boot shows with the phone off, replaced with iPhone's (tools/bootlogo/mkcharging.py)
// when the package carries them: charging (lpm.jpg) and battery empty (low_battery_alert.jpg). Each is optional on
// the phone (another bootloader version may not have it) and kept once as <name>.stock like the logo.
var extraPictures = map[string]bool{"lpm.jpg": true, "low_battery_alert.jpg": true}

type tarMember struct {
	Header tar.Header
	Data   []byte
}

func parsePictures(img []byte) ([]tarMember, error) {
	r := tar.NewReader(bytes.NewReader(img))
	var out []tarMember
	seen := map[string]bool{}
	for {
		h, e := r.Next()
		if e == io.EOF {
			break
		}
		if e != nil {
			return nil, fmt.Errorf("up_param is not a readable tar: %w", e)
		}
		if h.Typeflag != tar.TypeReg && h.Typeflag != tar.TypeRegA {
			return nil, fmt.Errorf("up_param holds a non-file member: %q", h.Name)
		}
		if h.Name == "" || strings.ContainsAny(h.Name, "/\\") || seen[h.Name] || h.Size < 0 || h.Size > 8<<20 {
			return nil, fmt.Errorf("up_param holds an unexpected member: %q", h.Name)
		}
		seen[h.Name] = true
		data, e := io.ReadAll(r)
		if e != nil {
			return nil, e
		}
		out = append(out, tarMember{Header: *h, Data: data})
	}
	if len(out) == 0 {
		return nil, errors.New("up_param holds no pictures")
	}
	return out, nil
}

func buildPictures(members []tarMember, size int) ([]byte, error) {
	var buf bytes.Buffer
	w := tar.NewWriter(&buf)
	for _, m := range members {
		h := m.Header
		h.Size = int64(len(m.Data))
		h.Format = tar.FormatUSTAR
		h.Typeflag = tar.TypeReg
		if e := w.WriteHeader(&h); e != nil {
			return nil, e
		}
		if _, e := w.Write(m.Data); e != nil {
			return nil, e
		}
	}
	if e := w.Close(); e != nil {
		return nil, e
	}
	if buf.Len() > size {
		return nil, fmt.Errorf("new pictures (%d bytes) do not fit up_param (%d)", buf.Len(), size)
	}
	return append(buf.Bytes(), make([]byte, size-buf.Len())...), nil
}

func memberIndex(ms []tarMember, name string) int {
	for i, m := range ms {
		if m.Header.Name == name {
			return i
		}
	}
	return -1
}

// verifyPictures: the new image parses back to exactly want (names, order, contents)
func verifyPictures(img []byte, want []tarMember) error {
	got, e := parsePictures(img)
	if e != nil {
		return e
	}
	if len(got) != len(want) {
		return errors.New("rebuilt up_param has a different member list")
	}
	for i := range got {
		if got[i].Header.Name != want[i].Header.Name || !bytes.Equal(got[i].Data, want[i].Data) {
			return fmt.Errorf("rebuilt up_param differs at %q", want[i].Header.Name)
		}
	}
	return nil
}

// withSplash: up_param with logo.jpg = splash; the phone's own logo.jpg kept once as logo.jpg.stock.
// changed false: it already shows this splash.
func withSplash(img, splash []byte) ([]byte, bool, error) { return withPictures(img, splash, nil) }

func isJPEG(p []byte) bool {
	return len(p) > 0 && len(p) <= splashMax && bytes.HasPrefix(p, []byte{0xff, 0xd8})
}

// withPictures: logo.jpg = splash (required), and each extra picture the phone has; originals kept once as .stock.
func withPictures(img, splash []byte, extra map[string][]byte) ([]byte, bool, error) {
	if !isJPEG(splash) {
		return nil, false, errors.New("splash is not a JPEG of at most 1 MiB")
	}
	for name, p := range extra {
		if !extraPictures[name] || !isJPEG(p) {
			return nil, false, fmt.Errorf("picture %q is not an allowed JPEG of at most 1 MiB", name)
		}
	}
	ms, e := parsePictures(img)
	if e != nil {
		return nil, false, e
	}
	if memberIndex(ms, splashName) < 0 {
		return nil, false, errors.New("up_param has no logo.jpg")
	}
	want := map[string][]byte{splashName: splash}
	for name, p := range extra {
		want[name] = p
	}
	changed := false
	for _, name := range []string{splashName, "lpm.jpg", "low_battery_alert.jpg"} {
		p, i := want[name], memberIndex(ms, name)
		if p == nil || i < 0 || bytes.Equal(ms[i].Data, p) {
			continue
		}
		if memberIndex(ms, name+stockSuffix) < 0 {
			stock := ms[i]
			stock.Header.Name = name + stockSuffix
			ms = append(ms, tarMember{Header: stock.Header, Data: append([]byte(nil), ms[i].Data...)})
		}
		ms[i].Data = p
		changed = true
	}
	if !changed {
		return img, false, nil
	}
	out, e := buildPictures(ms, len(img))
	if e != nil {
		return nil, false, e
	}
	return out, true, verifyPictures(out, ms)
}

// withoutSplash: every picture back from its .stock copy (logo.jpg and the extras), which is then removed.
// changed false: nothing saved to restore.
func withoutSplash(img []byte) ([]byte, bool, error) {
	ms, e := parsePictures(img)
	if e != nil {
		return nil, false, e
	}
	changed := false
	for _, name := range []string{splashName, "lpm.jpg", "low_battery_alert.jpg"} {
		s, i := memberIndex(ms, name+stockSuffix), memberIndex(ms, name)
		if s < 0 {
			continue
		}
		if i < 0 {
			return nil, false, fmt.Errorf("up_param has a saved %s but no %s", name, name)
		}
		ms[i].Data = ms[s].Data
		ms = append(ms[:s], ms[s+1:]...)
		changed = true
	}
	if !changed {
		return img, false, nil
	}
	out, e := buildPictures(ms, len(img))
	if e != nil {
		return nil, false, e
	}
	return out, true, verifyPictures(out, ms)
}

// applySplash / restoreSplash: read the whole partition, rewrite it, write it back with readback (transfer).
func applySplash(t Target, splash []byte, extra map[string][]byte, log func(string)) error {
	return rewritePictures(t, log, func(img []byte) ([]byte, bool, error) { return withPictures(img, splash, extra) },
		"Power-on pictures already set", "Setting iPhone's power-on and charging pictures (the phone's own kept for Jesvi-iOS-Restore)")
}
func restoreSplash(t Target, log func(string)) error {
	return rewritePictures(t, log, withoutSplash,
		"No saved power-on splash: Samsung's is in place", "Putting back the phone's own power-on splash")
}
func rewritePictures(t Target, log func(string), edit func([]byte) ([]byte, bool, error), same, doing string) error {
	size := capacities["up_param"]
	if size <= 0 {
		return errors.New("no up_param in the device profile")
	}
	img, e := readRange(t, 0, int(size))
	if e != nil {
		return e
	}
	out, changed, e := edit(img)
	if e != nil {
		return e
	}
	if !changed {
		log(same)
		return nil
	}
	log(doing)
	return transfer(t, bytes.NewReader(out), 0, size, checkedBytes(out))
}
