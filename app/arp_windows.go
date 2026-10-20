package main

import (
	"fmt"
	"net"
	"syscall"
	"unsafe"
)

// MIB_IPNETROW from iphlpapi.h.
type mibIPNetRow struct {
	Index       uint32
	PhysAddrLen uint32
	PhysAddr    [8]byte
	Addr        [4]byte // IPv4 address in network byte order
	Type        uint32
}

// arpTable maps IPv4 addresses to MAC addresses ("e0:31:9e:ab:cd:ef") from Windows' ARP table
// (GetIpNetTable). Devices appear there once we've contacted them.
func arpTable() map[string]string {
	out := map[string]string{}
	proc := syscall.NewLazyDLL("iphlpapi.dll").NewProc("GetIpNetTable")
	if proc.Find() != nil {
		return out
	}
	var size uint32
	proc.Call(0, uintptr(unsafe.Pointer(&size)), 0)
	if size == 0 {
		return out
	}
	buf := make([]byte, size)
	if r, _, _ := proc.Call(uintptr(unsafe.Pointer(&buf[0])), uintptr(unsafe.Pointer(&size)), 0); r != 0 {
		return out
	}
	n := *(*uint32)(unsafe.Pointer(&buf[0]))
	const rowSize = unsafe.Sizeof(mibIPNetRow{})
	for i := uint32(0); i < n; i++ {
		off := 4 + uintptr(i)*rowSize
		if off+rowSize > uintptr(len(buf)) {
			break
		}
		row := (*mibIPNetRow)(unsafe.Pointer(&buf[off]))
		if row.PhysAddrLen != 6 || row.Type == 2 { // 2 = invalid entry
			continue
		}
		m := row.PhysAddr
		ip := net.IPv4(row.Addr[0], row.Addr[1], row.Addr[2], row.Addr[3]).String()
		out[ip] = fmt.Sprintf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5])
	}
	return out
}
