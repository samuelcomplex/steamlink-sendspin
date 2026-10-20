// Finding Steam Links (devices with SSH open on the local network) and Music Assistant (its
// Sendspin server announces itself over mDNS as _sendspin-server._tcp).
package main

import (
	"context"
	"fmt"
	"net"
	"sort"
	"strings"
	"sync"
	"time"

	"golang.org/x/net/dns/dnsmessage"
)

type Device struct {
	IP        string `json:"ip"`
	Name      string `json:"name"`
	MAC       string `json:"mac,omitempty"`
	SteamLink bool   `json:"steamLink"` // Valve network card, or a name like a Steam Link
	Installed bool   `json:"installed"` // the Sendspin player answers on its port
}

type Server struct {
	Name string `json:"name"`
	URL  string `json:"url"`
}

// localNets returns the private IPv4 /24 networks this computer is on.
func localNets() []net.IP {
	var out []net.IP
	seen := map[string]bool{}
	ifs, _ := net.Interfaces()
	for _, ifc := range ifs {
		if ifc.Flags&net.FlagUp == 0 || ifc.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, _ := ifc.Addrs()
		for _, a := range addrs {
			ipn, ok := a.(*net.IPNet)
			if !ok {
				continue
			}
			ip := ipn.IP.To4()
			if ip == nil || !ip.IsPrivate() {
				continue
			}
			base := net.IPv4(ip[0], ip[1], ip[2], 0).To4()
			if !seen[base.String()] {
				seen[base.String()] = true
				out = append(out, base)
			}
		}
	}
	return out
}

func portOpen(ip string, port int, timeout time.Duration) bool {
	c, err := net.DialTimeout("tcp", net.JoinHostPort(ip, fmt.Sprint(port)), timeout)
	if err != nil {
		return false
	}
	c.Close()
	return true
}

// findDevices scans the local /24 networks for SSH, and names what it finds.
func findDevices(ctx context.Context) []Device {
	var (
		mu  sync.Mutex
		out = []Device{} // an empty list, not null, when nothing answers
		wg  sync.WaitGroup
	)
	sem := make(chan struct{}, 96)
	for _, base := range localNets() {
		for i := 1; i < 255; i++ {
			ip := net.IPv4(base[0], base[1], base[2], byte(i)).String()
			wg.Add(1)
			go func() {
				defer wg.Done()
				select {
				case sem <- struct{}{}:
				case <-ctx.Done():
					return
				}
				defer func() { <-sem }()
				if !portOpen(ip, 22, 600*time.Millisecond) {
					return
				}
				d := Device{IP: ip}
				lctx, cancel := context.WithTimeout(ctx, 1500*time.Millisecond)
				names, _ := net.DefaultResolver.LookupAddr(lctx, ip)
				cancel()
				if len(names) > 0 {
					d.Name = strings.TrimSuffix(names[0], ".")
				}
				d.SteamLink = strings.Contains(strings.ToLower(d.Name), "steamlink")
				d.Installed = portOpen(ip, 8928, 600*time.Millisecond)
				mu.Lock()
				out = append(out, d)
				mu.Unlock()
			}()
		}
	}
	wg.Wait()

	// Every device we just contacted is in the ARP table now. Steam Links have Valve network
	// hardware (prefix E0:31:9E, registered in 2015 for it), and are named steamlink-XXXX after
	// the end of that address when the network has no name for them.
	macs := arpTable()
	for i := range out {
		d := &out[i]
		d.MAC = macs[d.IP]
		if strings.HasPrefix(d.MAC, valvePrefix) {
			d.SteamLink = true
			if d.Name == "" && len(d.MAC) == 17 {
				d.Name = "steamlink-" + strings.ToUpper(strings.ReplaceAll(d.MAC[12:], ":", ""))
			}
		}
	}

	sort.Slice(out, func(i, j int) bool {
		a, b := out[i], out[j]
		if a.SteamLink != b.SteamLink {
			return a.SteamLink
		}
		return ipLess(a.IP, b.IP)
	})
	return out
}

func ipLess(a, b string) bool {
	x, y := net.ParseIP(a).To4(), net.ParseIP(b).To4()
	for i := 0; i < 4; i++ {
		if x[i] != y[i] {
			return x[i] < y[i]
		}
	}
	return false
}

const serverService = "_sendspin-server._tcp.local."

// Valve Corporation's MAC address prefix (IEEE MA-L E0-31-9E).
const valvePrefix = "e0:31:9e:"

// findServers asks the network for Sendspin servers (Music Assistant) over mDNS. The query goes
// out from an ordinary port, so responders answer us directly (RFC 6762 legacy unicast).
func findServers(ctx context.Context) []Server {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{})
	if err != nil {
		return []Server{}
	}
	defer conn.Close()

	b := dnsmessage.NewBuilder(nil, dnsmessage.Header{})
	b.StartQuestions()
	b.Question(dnsmessage.Question{Name: dnsmessage.MustNewName(serverService),
		Type: dnsmessage.TypePTR, Class: dnsmessage.ClassINET | 1<<15}) // QU: unicast reply please
	query, _ := b.Finish()
	dst := &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: 5353}

	type inst struct {
		name, path, target string
		port               uint16
		ip                 string
	}
	insts := map[string]*inst{}
	get := func(n string) *inst {
		if insts[n] == nil {
			insts[n] = &inst{path: "/sendspin"}
		}
		return insts[n]
	}
	hostIPs := map[string]string{}

	deadline := time.Now().Add(2500 * time.Millisecond)
	if d, ok := ctx.Deadline(); ok && d.Before(deadline) {
		deadline = d
	}
	for _, at := range []time.Duration{0, 700 * time.Millisecond} {
		time.AfterFunc(at, func() { conn.WriteTo(query, dst) })
	}
	buf := make([]byte, 9000)
	conn.SetReadDeadline(deadline)
	for {
		n, from, err := conn.ReadFromUDP(buf)
		if err != nil {
			break
		}
		var p dnsmessage.Parser
		if _, err := p.Start(buf[:n]); err != nil {
			continue
		}
		p.SkipAllQuestions()
		var rrs []dnsmessage.Resource
		for _, get := range []func() ([]dnsmessage.Resource, error){p.AllAnswers, p.AllAuthorities, p.AllAdditionals} {
			r, err := get()
			if err != nil {
				break
			}
			rrs = append(rrs, r...)
		}
		for _, rr := range rrs {
			name := rr.Header.Name.String()
			switch body := rr.Body.(type) {
			case *dnsmessage.PTRResource:
				if strings.EqualFold(name, serverService) {
					i := get(body.PTR.String())
					if i.ip == "" {
						i.ip = from.IP.String()
					}
				}
			case *dnsmessage.SRVResource:
				i := get(name)
				i.port, i.target = body.Port, body.Target.String()
				if i.ip == "" {
					i.ip = from.IP.String()
				}
			case *dnsmessage.TXTResource:
				i := get(name)
				for _, kv := range body.TXT {
					k, v, _ := strings.Cut(kv, "=")
					switch strings.ToLower(k) {
					case "path":
						i.path = v
					case "name":
						i.name = v
					}
				}
			case *dnsmessage.AResource:
				hostIPs[name] = net.IP(body.A[:]).String()
			}
		}
	}

	out := []Server{}
	seen := map[string]bool{}
	for key, i := range insts {
		if !strings.HasSuffix(strings.ToLower(key), serverService) {
			continue
		}
		ip := i.ip
		if a, ok := hostIPs[i.target]; ok {
			ip = a
		}
		port := i.port
		if port == 0 {
			port = 8927
		}
		if ip == "" {
			continue
		}
		url := fmt.Sprintf("ws://%s:%d%s", ip, port, i.path)
		if seen[url] {
			continue
		}
		seen[url] = true
		name := i.name
		if name == "" {
			name = "Sendspin server at " + ip
		}
		out = append(out, Server{Name: name, URL: url})
	}
	sort.Slice(out, func(a, b int) bool { return out[a].URL < out[b].URL })
	return out
}

// normalizeServer turns what a person types ("192.168.1.6", "192.168.1.6:8927",
// "ws://...") into a Sendspin URL.
func normalizeServer(s string) string {
	s = strings.TrimSpace(s)
	if s == "" || strings.HasPrefix(s, "ws://") || strings.HasPrefix(s, "wss://") {
		return s
	}
	s = strings.TrimPrefix(strings.TrimPrefix(s, "http://"), "https://")
	s = strings.TrimSuffix(s, "/")
	if !strings.Contains(s, ":") {
		s += ":8927"
	}
	if !strings.Contains(s, "/") {
		s += "/sendspin"
	}
	return "ws://" + s
}
