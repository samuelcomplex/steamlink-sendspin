package main

import (
	"bufio"
	"os"
	"strings"
)

// arpTable maps IPv4 addresses to MAC addresses ("e0:31:9e:ab:cd:ef") from the kernel's
// neighbour table. Devices appear there once we've contacted them.
func arpTable() map[string]string {
	out := map[string]string{}
	f, err := os.Open("/proc/net/arp")
	if err != nil {
		return out
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Scan() // header
	for sc.Scan() {
		fields := strings.Fields(sc.Text())
		if len(fields) >= 4 && fields[3] != "00:00:00:00:00:00" {
			out[fields[0]] = strings.ToLower(fields[3])
		}
	}
	return out
}
