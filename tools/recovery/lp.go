// SPDX-License-Identifier: MIT
package main

// Android dynamic-partition ("LP") metadata at the head of physical super:
// reserved 4 KiB, two copies of the geometry, then the primary and backup
// metadata slots. Android custom-ROM ZIPs (dynamic_partitions_op_list) load this
// before rebuilding their groups, so the restore ZIP must put a valid copy back
// after iOS has overwritten it. The encoder is checked byte-for-byte against a
// real Samsung super head in lp_test.go.

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
)

const (
	lpGeometryMagic = 0x616c4467
	lpHeaderMagic   = 0x414c5030
	lpReserved      = 4096
	lpGeometrySize  = 4096
	lpSector        = 512
	lpRegion        = 1 << 20 // bytes of super saved/restored; covers all metadata copies
)

type lpPartition struct {
	Name                               string
	Attributes, FirstExtent, NumExtent uint32
	Group                              uint32
}
type lpExtent struct {
	Sectors uint64
	Type    uint32
	Data    uint64
	Source  uint32
}
type lpGroup struct {
	Name  string
	Flags uint32
	Max   uint64
}
type lpBlockDevice struct {
	FirstSector     uint64
	Alignment       uint32
	AlignmentOffset uint32
	Size            uint64
	Name            string
	Flags           uint32
}
type lpMetadata struct {
	MaxSize, Slots, BlockSize uint32
	Major, Minor              uint16
	HeaderFlags               uint32
	Reserved                  []byte // super [0, 4 KiB); ignored by liblp, kept for fidelity
	Partitions                []lpPartition
	Extents                   []lpExtent
	Groups                    []lpGroup
	Devices                   []lpBlockDevice
}

func lpName(b []byte) (string, error) {
	n := bytes.IndexByte(b, 0)
	if n < 0 {
		n = len(b)
	}
	for _, c := range b[:n] {
		if c < 0x21 || c > 0x7e {
			return "", errors.New("invalid LP name")
		}
	}
	for _, c := range b[n:] {
		if c != 0 {
			return "", errors.New("LP name padding not zero")
		}
	}
	return string(b[:n]), nil
}
func putName(b []byte, s string) error {
	if len(s) == 0 || len(s) > len(b) {
		return errors.New("invalid LP name length")
	}
	copy(b, s)
	return nil
}
func (m *lpMetadata) headerSize() uint32 {
	if m.Minor >= 2 {
		return 256
	}
	return 128
}

// parseLP checks the primary geometry/metadata copy of a 1 MiB super head.
func parseLP(region []byte) (*lpMetadata, error) {
	if len(region) < lpReserved+2*lpGeometrySize {
		return nil, errors.New("short LP region")
	}
	g := region[lpReserved : lpReserved+52]
	le := binary.LittleEndian
	if le.Uint32(g[0:4]) != lpGeometryMagic || le.Uint32(g[4:8]) != 52 {
		return nil, errors.New("no Android LP geometry")
	}
	check := append(append(append([]byte{}, g[:8]...), make([]byte, 32)...), g[40:52]...)
	if sum := sha256.Sum256(check); !bytes.Equal(sum[:], g[8:40]) {
		return nil, errors.New("LP geometry checksum mismatch")
	}
	m := &lpMetadata{MaxSize: le.Uint32(g[40:44]), Slots: le.Uint32(g[44:48]), BlockSize: le.Uint32(g[48:52]), Reserved: append([]byte{}, region[:lpReserved]...)}
	if m.MaxSize == 0 || m.MaxSize%lpSector != 0 || m.MaxSize > 1<<20 || m.Slots == 0 || m.Slots > 4 || m.BlockSize == 0 || m.BlockSize%lpSector != 0 {
		return nil, errors.New("unsupported LP geometry")
	}
	start := uint64(lpReserved + 2*lpGeometrySize)
	if start+2*uint64(m.Slots)*uint64(m.MaxSize) > uint64(len(region)) {
		return nil, errors.New("LP metadata exceeds saved region")
	}
	h := region[start : start+uint64(m.MaxSize)]
	if le.Uint32(h[0:4]) != lpHeaderMagic {
		return nil, errors.New("no LP metadata header")
	}
	m.Major, m.Minor = le.Uint16(h[4:6]), le.Uint16(h[6:8])
	if m.Major != 10 || m.Minor > 2 {
		return nil, fmt.Errorf("unsupported LP version %d.%d", m.Major, m.Minor)
	}
	hs := le.Uint32(h[8:12])
	if hs != m.headerSize() {
		return nil, errors.New("LP header size mismatch")
	}
	hcopy := append([]byte{}, h[:hs]...)
	clear(hcopy[12:44])
	if sum := sha256.Sum256(hcopy); !bytes.Equal(sum[:], h[12:44]) {
		return nil, errors.New("LP header checksum mismatch")
	}
	ts := le.Uint32(h[44:48])
	if uint64(hs)+uint64(ts) > uint64(m.MaxSize) {
		return nil, errors.New("LP tables exceed slot")
	}
	tables := h[hs : hs+ts]
	if sum := sha256.Sum256(tables); !bytes.Equal(sum[:], h[48:80]) {
		return nil, errors.New("LP tables checksum mismatch")
	}
	if hs == 256 {
		m.HeaderFlags = le.Uint32(h[128:132])
	}
	table := func(at, size int, each func([]byte) error) error {
		off, n, es := le.Uint32(h[at:at+4]), le.Uint32(h[at+4:at+8]), le.Uint32(h[at+8:at+12])
		if int(es) != size || uint64(off)+uint64(n)*uint64(es) > uint64(ts) {
			return errors.New("invalid LP table descriptor")
		}
		for i := uint32(0); i < n; i++ {
			if e := each(tables[off+i*es : off+(i+1)*es]); e != nil {
				return e
			}
		}
		return nil
	}
	e := table(80, 52, func(b []byte) error {
		name, e := lpName(b[:36])
		m.Partitions = append(m.Partitions, lpPartition{name, le.Uint32(b[36:40]), le.Uint32(b[40:44]), le.Uint32(b[44:48]), le.Uint32(b[48:52])})
		return e
	})
	if e == nil {
		e = table(92, 24, func(b []byte) error {
			m.Extents = append(m.Extents, lpExtent{le.Uint64(b[0:8]), le.Uint32(b[8:12]), le.Uint64(b[12:20]), le.Uint32(b[20:24])})
			return nil
		})
	}
	if e == nil {
		e = table(104, 48, func(b []byte) error {
			name, e := lpName(b[:36])
			m.Groups = append(m.Groups, lpGroup{name, le.Uint32(b[36:40]), le.Uint64(b[40:48])})
			return e
		})
	}
	if e == nil {
		e = table(116, 64, func(b []byte) error {
			name, e := lpName(b[24:60])
			m.Devices = append(m.Devices, lpBlockDevice{le.Uint64(b[0:8]), le.Uint32(b[8:12]), le.Uint32(b[12:16]), le.Uint64(b[16:24]), name, le.Uint32(b[60:64])})
			return e
		})
	}
	if e != nil {
		return nil, e
	}
	if len(m.Devices) != 1 || m.Devices[0].Name != "super" || len(m.Groups) == 0 || m.Groups[0].Name != "default" {
		return nil, errors.New("LP metadata is not a single-super layout")
	}
	for _, p := range m.Partitions {
		if uint64(p.FirstExtent)+uint64(p.NumExtent) > uint64(len(m.Extents)) || int(p.Group) >= len(m.Groups) {
			return nil, errors.New("LP partition references are invalid")
		}
	}
	if m.Devices[0].FirstSector*lpSector > lpRegion || m.Devices[0].FirstSector*lpSector < start+2*uint64(m.Slots)*uint64(m.MaxSize) {
		return nil, errors.New("LP first logical sector outside saved region")
	}
	return m, nil
}

// encode returns the bytes of super [0, first logical sector): every geometry
// and metadata copy, exactly as liblp writes them.
func (m *lpMetadata) encode() ([]byte, error) {
	if len(m.Devices) != 1 || len(m.Groups) == 0 || m.Major != 10 || m.Minor > 2 {
		return nil, errors.New("unsupported LP metadata")
	}
	le := binary.LittleEndian
	var tables []byte
	part := len(tables)
	for _, p := range m.Partitions {
		b := make([]byte, 52)
		if e := putName(b[:36], p.Name); e != nil {
			return nil, e
		}
		le.PutUint32(b[36:], p.Attributes)
		le.PutUint32(b[40:], p.FirstExtent)
		le.PutUint32(b[44:], p.NumExtent)
		le.PutUint32(b[48:], p.Group)
		tables = append(tables, b...)
	}
	ext := len(tables)
	for _, x := range m.Extents {
		b := make([]byte, 24)
		le.PutUint64(b[0:], x.Sectors)
		le.PutUint32(b[8:], x.Type)
		le.PutUint64(b[12:], x.Data)
		le.PutUint32(b[20:], x.Source)
		tables = append(tables, b...)
	}
	grp := len(tables)
	for _, g := range m.Groups {
		b := make([]byte, 48)
		if e := putName(b[:36], g.Name); e != nil {
			return nil, e
		}
		le.PutUint32(b[36:], g.Flags)
		le.PutUint64(b[40:], g.Max)
		tables = append(tables, b...)
	}
	dev := len(tables)
	for _, d := range m.Devices {
		b := make([]byte, 64)
		le.PutUint64(b[0:], d.FirstSector)
		le.PutUint32(b[8:], d.Alignment)
		le.PutUint32(b[12:], d.AlignmentOffset)
		le.PutUint64(b[16:], d.Size)
		if e := putName(b[24:60], d.Name); e != nil {
			return nil, e
		}
		le.PutUint32(b[60:], d.Flags)
		tables = append(tables, b...)
	}
	hs := m.headerSize()
	if uint64(hs)+uint64(len(tables)) > uint64(m.MaxSize) {
		return nil, errors.New("LP metadata exceeds slot size")
	}
	h := make([]byte, hs)
	le.PutUint32(h[0:], lpHeaderMagic)
	le.PutUint16(h[4:], m.Major)
	le.PutUint16(h[6:], m.Minor)
	le.PutUint32(h[8:], hs)
	le.PutUint32(h[44:], uint32(len(tables)))
	ts := sha256.Sum256(tables)
	copy(h[48:80], ts[:])
	desc := func(at, off, n, size int) {
		le.PutUint32(h[at:], uint32(off))
		le.PutUint32(h[at+4:], uint32(n))
		le.PutUint32(h[at+8:], uint32(size))
	}
	desc(80, part, len(m.Partitions), 52)
	desc(92, ext, len(m.Extents), 24)
	desc(104, grp, len(m.Groups), 48)
	desc(116, dev, len(m.Devices), 64)
	if hs == 256 {
		le.PutUint32(h[128:], m.HeaderFlags)
	}
	hsum := sha256.Sum256(h)
	copy(h[12:44], hsum[:])
	blob := append(h, tables...)

	size := m.Devices[0].FirstSector * lpSector
	start := uint64(lpReserved + 2*lpGeometrySize)
	if size > lpRegion || size < start+2*uint64(m.Slots)*uint64(m.MaxSize) {
		return nil, errors.New("LP first logical sector does not fit metadata")
	}
	out := make([]byte, size)
	if len(m.Reserved) == lpReserved {
		copy(out, m.Reserved)
	}
	g := make([]byte, 52)
	le.PutUint32(g[0:], lpGeometryMagic)
	le.PutUint32(g[4:], 52)
	le.PutUint32(g[40:], m.MaxSize)
	le.PutUint32(g[44:], m.Slots)
	le.PutUint32(g[48:], m.BlockSize)
	gsum := sha256.Sum256(g)
	copy(g[8:40], gsum[:])
	copy(out[lpReserved:], g)
	copy(out[lpReserved+lpGeometrySize:], g)
	for slot := uint64(0); slot < uint64(m.Slots)*2; slot++ {
		copy(out[start+slot*uint64(m.MaxSize):], blob)
	}
	return out, nil
}

// emptied keeps geometry, groups and the super block device but drops every
// logical partition, the state an Android ROM ZIP expects before it adds its own.
func (m *lpMetadata) emptied() *lpMetadata {
	c := *m
	c.Partitions, c.Extents = nil, nil
	c.Reserved = append([]byte{}, m.Reserved...)
	c.Groups = append([]lpGroup{}, m.Groups...)
	c.Devices = append([]lpBlockDevice{}, m.Devices...)
	return &c
}

// withPlaceholders adds system, vendor, product and odm, 1 MiB each, to the first group after "default": TWRP's
// Install unmounts /system_root before it runs any ROM ZIP and stops ("Unable to find partition for path
// '/system_root'") when super holds no such partition (2026-11-20). The ROM's own update_dynamic_partitions replaces
// them at once. Read-only, linear, one after the other from the first logical sector.
func (m *lpMetadata) withPlaceholders() *lpMetadata {
	c := *m
	if len(c.Groups) < 2 || len(c.Devices) == 0 {
		return &c
	}
	c.Partitions = append([]lpPartition{}, m.Partitions...)
	c.Extents = append([]lpExtent{}, m.Extents...)
	at := c.Devices[0].FirstSector
	for _, name := range []string{"system", "vendor", "product", "odm"} {
		c.Partitions = append(c.Partitions, lpPartition{Name: name, Attributes: 1, FirstExtent: uint32(len(c.Extents)), NumExtent: 1, Group: 1})
		c.Extents = append(c.Extents, lpExtent{Sectors: 2048, Type: 0, Data: at, Source: 0})
		at += 2048
	}
	return &c
}

// onlyPlaceholders: super holds exactly what withPlaceholders left (a restore already ran; no ROM yet).
func (m *lpMetadata) onlyPlaceholders() bool {
	if len(m.Partitions) != 4 || len(m.Extents) != 4 {
		return false
	}
	for i, name := range []string{"system", "vendor", "product", "odm"} {
		p := m.Partitions[i]
		if p.Name != name || p.NumExtent != 1 || m.Extents[p.FirstExtent].Sectors != 2048 {
			return false
		}
	}
	return true
}

// AndroidSuper is the measured stock layout, used when no saved copy exists.
type AndroidSuper struct {
	MetadataMaxSize    uint32         `json:"metadata_max_size"`
	MetadataSlots      uint32         `json:"metadata_slots"`
	LogicalBlockSize   uint32         `json:"logical_block_size"`
	Alignment          uint32         `json:"alignment"`
	FirstLogicalSector uint64         `json:"first_logical_sector"`
	Groups             []AndroidGroup `json:"groups"`
}
type AndroidGroup struct {
	Name    string `json:"name"`
	MaxSize uint64 `json:"max_size"`
}

func (a AndroidSuper) metadata(superSize int64) *lpMetadata {
	m := &lpMetadata{MaxSize: a.MetadataMaxSize, Slots: a.MetadataSlots, BlockSize: a.LogicalBlockSize, Major: 10,
		Groups:  []lpGroup{{Name: "default"}},
		Devices: []lpBlockDevice{{FirstSector: a.FirstLogicalSector, Alignment: a.Alignment, Size: uint64(superSize), Name: "super"}}}
	for _, g := range a.Groups {
		m.Groups = append(m.Groups, lpGroup{Name: g.Name, Max: g.MaxSize})
	}
	return m
}

// The installer saves Android's original super head in super's last 2 MiB
// (unused by iOS) so the restore ZIP can put the exact layout back.
const stashMagic = "JESVILP1"

func encodeStash(region []byte, superSize int64) []byte {
	h := make([]byte, 4096)
	copy(h, stashMagic)
	binary.LittleEndian.PutUint64(h[8:], uint64(len(region)))
	sum := sha256.Sum256(region)
	copy(h[16:48], sum[:])
	binary.LittleEndian.PutUint64(h[48:], uint64(superSize))
	return append(h, region...)
}
func decodeStash(b []byte, superSize int64) (*lpMetadata, error) {
	if len(b) < 4096+lpRegion || string(b[:8]) != stashMagic || binary.LittleEndian.Uint64(b[8:]) != lpRegion || binary.LittleEndian.Uint64(b[48:]) != uint64(superSize) {
		return nil, errors.New("no saved Android partition map")
	}
	region := b[4096 : 4096+lpRegion]
	if sum := sha256.Sum256(region); !bytes.Equal(sum[:], b[16:48]) {
		return nil, errors.New("saved Android partition map checksum mismatch")
	}
	m, e := parseLP(region)
	if e != nil {
		return nil, e
	}
	if m.Devices[0].Size != uint64(superSize) {
		return nil, errors.New("saved Android partition map is for another super size")
	}
	return m, nil
}
