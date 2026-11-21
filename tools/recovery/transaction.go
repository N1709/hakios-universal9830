// SPDX-License-Identifier: MIT
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
)

// The transaction core cannot name, resize or repartition any disk. The platform
// supplies four already-checked descriptors; tests supply independent files.
type Target interface {
	io.ReaderAt
	io.WriterAt
	Sync() error
}
type Device interface {
	OpenWritable(string) (Target, error)
	CheckSafe() error
	Close() error
}
type zeroReader struct{}

func (zeroReader) Read(p []byte) (int, error) { clear(p); return len(p), nil }
func transfer(target Target, input io.Reader, offset, length int64, want string) error {
	h := sha256.New()
	buf := make([]byte, 1<<20)
	at := int64(0)
	for at < length {
		take := int64(len(buf))
		if length-at < take {
			take = length - at
		}
		n, e := io.ReadFull(input, buf[:take])
		if e != nil {
			return fmt.Errorf("short source: %w", e)
		}
		written, e := target.WriteAt(buf[:n], offset+at)
		if e != nil {
			return e
		}
		if written != n {
			return io.ErrShortWrite
		}
		h.Write(buf[:n])
		at += int64(n)
		ui.advance(int64(n))
	}
	// Even after prevalidation, detect changed/truncated sources during writing.
	if hex.EncodeToString(h.Sum(nil)) != want {
		return errors.New("source changed during write")
	}
	if e := target.Sync(); e != nil {
		return e
	}
	readback := sha256.New()
	if _, e := io.CopyN(io.MultiWriter(readback, progressWriter{}), io.NewSectionReader(target, offset, length), length); e != nil {
		return e
	}
	if hex.EncodeToString(readback.Sum(nil)) != want {
		return errors.New("readback checksum mismatch")
	}
	return nil
}

// progressWriter moves the bar while written data is read back.
type progressWriter struct{}

func (progressWriter) Write(p []byte) (int, error) { ui.advance(int64(len(p))); return len(p), nil }

func initialized(target io.ReaderAt, size int64) bool {
	h := make([]byte, 512)
	if _, e := target.ReadAt(h, 1024); e != nil {
		return false
	}
	return validHFS(h, size)
}
func perform(b *Bundle, device Device, mode string, log func(string)) error {
	if mode != "fresh" && mode != "update" {
		return errors.New("installation mode must be explicit")
	}
	extents, e := parseKit(b.Kit, dataBytes)
	if e != nil {
		return e
	}
	if e = device.CheckSafe(); e != nil {
		return e
	}
	// Open all required descriptors exclusively before the first write.
	names := []string{"super", "recovery", "boot"}
	if mode == "fresh" {
		names = append(names, "userdata")
	}
	if b.Splash != nil {
		names = append(names, "up_param")
	}
	targets := make(map[string]Target)
	for _, name := range names {
		t, e := device.OpenWritable(name)
		if e != nil {
			return e
		}
		targets[name] = t
	}
	if mode == "update" {
		// Backend checked existing data read-only before confirmation; check again at commit time.
		if checker, ok := device.(interface{ CheckData() error }); !ok {
			return errors.New("missing update data check")
		} else if e = checker.CheckData(); e != nil {
			return e
		}
	}
	if e = device.CheckSafe(); e != nil {
		return e
	}
	// The bar covers every byte written and read back, so it is one smooth 0-100 % for the whole install.
	work := 2 * (b.Config.Files["system.apfs"].Bytes + b.Config.Files["recovery.img"].Bytes + b.Config.Files["boot.img"].Bytes)
	steps := ui.step + 4
	if mode == "fresh" {
		steps++
		for _, x := range extents {
			work += 2 * x.Length
		}
	}
	if b.Splash != nil {
		steps++
	}
	ui.plan(steps, work)
	writeImage := func(name string) error {
		if e := device.CheckSafe(); e != nil {
			return e
		}

		m := b.Config.Files[name]
		target := targets[m.Partition]
		if e := target.Sync(); e != nil {
			return e
		}
		existing := sha256.New()
		if _, e := io.CopyN(existing, io.NewSectionReader(target, m.Offset, m.Bytes), m.Bytes); e != nil {
			return e
		}
		if hex.EncodeToString(existing.Sum(nil)) == m.SHA256 {
			log("Already matches verified " + m.Partition + "; skipping write")
			ui.advance(2 * m.Bytes)
			return nil
		}
		var r io.ReadCloser
		var e error
		if data, ok := b.Staged[name]; ok {
			r = io.NopCloser(bytes.NewReader(data))
		} else {
			r, e = b.Files[name].Open()
		}
		if e != nil {
			return e
		}
		defer r.Close()
		log("Writing and verifying " + m.Partition)
		return transfer(targets[m.Partition], r, m.Offset, m.Bytes, m.SHA256)
	}
	ui.begin("Saving the Android partition map")
	saved := ui.total
	ui.total = 0 // small write: no bar
	e = saveAndroidMap(b, targets["super"], log)
	ui.total = saved
	if e != nil {
		return fmt.Errorf("saving Android partition map: %w", e)
	}
	ui.begin("Installing the iOS 16.5 system")
	if e = writeImage("system.apfs"); e != nil {
		return fmt.Errorf("system: %w", e)
	}
	if mode == "fresh" {
		ui.begin("Preparing a clean data partition")
		log("Explicit fresh install: initializing HFSX userdata (not a secure erase)")
		for _, x := range extents {
			if e = device.CheckSafe(); e != nil {
				return e
			}
			var input io.Reader = io.LimitReader(zeroReader{}, x.Length)
			if x.Data != nil {
				input = bytes.NewReader(x.Data)
			}
			h := sha256.New()
			var again io.Reader = io.LimitReader(zeroReader{}, x.Length)
			if x.Data != nil {
				again = bytes.NewReader(x.Data)
			}
			if _, e = io.Copy(h, again); e != nil {
				return e
			}
			if e = transfer(targets["userdata"], input, x.Offset, x.Length, hex.EncodeToString(h.Sum(nil))); e != nil {
				return fmt.Errorf("userdata: %w", e)
			}
		}
		if !initialized(targets["userdata"], dataBytes) {
			return errors.New("new HFS geometry verification failed")
		}
	}
	ui.begin("Installing the Jesvi iOS runtime")
	if e = writeImage("recovery.img"); e != nil {
		return fmt.Errorf("recovery: %w", e)
	}
	ui.begin("Installing the boot loader")
	if e = writeImage("boot.img"); e != nil {
		return fmt.Errorf("boot: %w", e)
	}
	// last: Samsung's power-on splash (iOS is installed whatever happens here)
	if b.Splash != nil {
		if e = device.CheckSafe(); e != nil {
			return e
		}
		ui.begin("Installing the Apple logo at power-on")
		saved := ui.total
		ui.total = 0
		e = applySplash(targets["up_param"], b.Splash, b.Pictures, log)
		ui.total = saved
		if e != nil {
			return fmt.Errorf("power-on splash (iOS itself is installed; Samsung's splash may need its BL from Odin): %w", e)
		}
	}
	log("All written ranges passed readback. No reboot requested.")
	return nil
}

func checkedBytes(data []byte) string {
	h := sha256.Sum256(data)
	return hex.EncodeToString(h[:])
}
func readRange(t io.ReaderAt, at int64, n int) ([]byte, error) {
	b := make([]byte, n)
	_, e := t.ReadAt(b, at)
	return b, e
}

// Before iOS overwrites super, keep Android's partition map (1 MiB) in super's
// free tail for the restore ZIP. Phones already on iOS keep their saved copy.
func saveAndroidMap(b *Bundle, super Target, log func(string)) error {
	at := b.Config.StashOffset
	if at < systemMax || at+4096+lpRegion > capacities["super"] {
		return errors.New("stash outside super's free tail")
	}
	head, e := readRange(super, 0, lpRegion)
	if e != nil {
		return e
	}
	if _, e = parseLP(head); e != nil {
		log("No Android partition map at the start of super (already iOS); keeping any saved copy")
		return nil
	}
	stash := encodeStash(head, capacities["super"])
	if old, e := readRange(super, at, len(stash)); e == nil && bytes.Equal(old, stash) {
		log("Android partition map already saved")
		return nil
	}
	log("Saving the Android partition map for Jesvi-iOS-Restore")
	return transfer(super, bytes.NewReader(stash), at, int64(len(stash)), checkedBytes(stash))
}

// restoreState: true when super already has an empty Android map; an error when
// super holds a live Android layout or something unrecognized.
func restoreState(head []byte) (bool, error) {
	if m, e := parseLP(head); e == nil {
		if len(m.Partitions) != 0 && !m.onlyPlaceholders() {
			return false, errors.New("super already holds Android partitions; nothing to restore")
		}
		return true, nil
	}
	if len(head) < 36 || string(head[32:36]) != "NXSB" {
		return false, errors.New("super holds neither iOS nor an Android map; refusing to guess")
	}
	return false, nil
}

// restoreAndroid puts a valid, empty Android partition map back at the head of
// super and clears the HFSX headers on userdata, so TWRP's Format Data and any
// Android ROM ZIP work again. BOOT and recovery (TWRP, running now) are untouched.
func restoreAndroid(b *Bundle, device Device, log func(string)) error {
	if e := device.CheckSafe(); e != nil {
		return e
	}
	super, e := device.OpenWritable("super")
	if e != nil {
		return e
	}
	data, e := device.OpenWritable("userdata")
	if e != nil {
		return e
	}
	if e = device.CheckSafe(); e != nil {
		return e
	}
	size := capacities["super"]
	head, e := readRange(super, 0, lpRegion)
	if e != nil {
		return e
	}
	done, e := restoreState(head)
	if e != nil {
		return e
	}
	if done {
		log("Android partition map already restored; skipping super")
	} else {
		var m *lpMetadata
		saved, e := readRange(super, b.Config.StashOffset, 4096+lpRegion)
		if e == nil {
			m, e = decodeStash(saved, size)
		}
		if e == nil {
			log("Restoring the Android partition map saved at install time")
			m = m.emptied()
		} else {
			log("No saved map (" + e.Error() + "); using the measured stock layout")
			m = b.Config.AndroidSuper.metadata(size)
		}
		region, e := m.withPlaceholders().encode()
		if e != nil {
			return e
		}
		region = append(region, make([]byte, lpRegion-len(region))...)
		if e = transfer(super, bytes.NewReader(region), 0, lpRegion, checkedBytes(region)); e != nil {
			return fmt.Errorf("super: %w", e)
		}
	}
	dsize := capacities["userdata"]
	h, e := readRange(data, 1024, 2)
	if e != nil {
		return e
	}
	if string(h) == "HX" || string(h) == "H+" {
		log("Clearing iOS data headers on userdata (Format Data will finish the wipe)")
		zero := make([]byte, 1<<20)
		for _, at := range []int64{0, dsize - 1<<20} {
			if e = device.CheckSafe(); e != nil {
				return e
			}
			if e = transfer(data, bytes.NewReader(zero), at, 1<<20, checkedBytes(zero)); e != nil {
				return fmt.Errorf("userdata: %w", e)
			}
		}
	} else {
		log("userdata holds no iOS volume; leaving it alone")
	}
	// Samsung's own power-on splash back, when the installer saved it (splash.go)
	if _, ok := capacities["up_param"]; ok {
		up, e := device.OpenWritable("up_param")
		if e != nil {
			return e
		}
		if e = device.CheckSafe(); e != nil {
			return e
		}
		if e = restoreSplash(up, log); e != nil {
			return fmt.Errorf("power-on splash: %w", e)
		}
	}
	return nil
}
