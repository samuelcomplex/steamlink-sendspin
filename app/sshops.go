// SSH operations against a Steam Link: install (streams the payload over stdin) and the
// device-side ctl.sh commands.
package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"strings"
	"time"

	"golang.org/x/crypto/ssh"
)

// Commands the UI may run, mapped to ctl.sh subcommands on the device.
var deviceCommands = map[string]bool{
	"status": true, "log": true, "trace": true, "restart": true,
	"ui": true, "speaker": true, "uninstall": true,
}

func sshConfig(password string) *ssh.ClientConfig {
	// The Steam Link's sshd is old: offer every algorithm Go supports, including the legacy ones.
	var kex, ciphers, macs, hostKeys []string
	for _, set := range []ssh.Algorithms{ssh.SupportedAlgorithms(), ssh.InsecureAlgorithms()} {
		kex = append(kex, set.KeyExchanges...)
		ciphers = append(ciphers, set.Ciphers...)
		macs = append(macs, set.MACs...)
		hostKeys = append(hostKeys, set.HostKeys...)
	}
	return &ssh.ClientConfig{
		User: "root",
		Auth: []ssh.AuthMethod{
			ssh.Password(password),
			ssh.KeyboardInteractive(func(_, _ string, qs []string, _ []bool) ([]string, error) {
				ans := make([]string, len(qs))
				for i := range qs {
					ans[i] = password
				}
				return ans, nil
			}),
		},
		// A device on the local network identified by the address the user picked. There's no
		// prior key to compare against on first use, as with ssh's own first-connect prompt.
		HostKeyCallback:   ssh.InsecureIgnoreHostKey(),
		HostKeyAlgorithms: hostKeys,
		Config:            ssh.Config{KeyExchanges: kex, Ciphers: ciphers, MACs: macs},
		Timeout:           8 * time.Second,
	}
}

func dial(host, password string) (*ssh.Client, error) {
	addr := net.JoinHostPort(host, "22")
	c, err := ssh.Dial("tcp", addr, sshConfig(password))
	if err != nil {
		msg := err.Error()
		switch {
		case strings.Contains(msg, "unable to authenticate"):
			return nil, errors.New("the Steam Link rejected the password")
		case strings.Contains(msg, "connection refused"):
			return nil, fmt.Errorf("%s isn't accepting SSH connections: SSH isn't enabled on it, or it's another device", host)
		case strings.Contains(msg, "timeout"), strings.Contains(msg, "no route"):
			return nil, fmt.Errorf("couldn't reach %s: check the address and that the Steam Link is on", host)
		}
		return nil, fmt.Errorf("couldn't connect to %s: %v", host, err)
	}
	return c, nil
}

// run executes cmd on the device, streaming combined output to out. A dropped connection after a
// reboot command is reported as such rather than as an error.
func run(ctx context.Context, host, password, cmd string, stdin io.Reader, out io.Writer) error {
	c, err := dial(host, password)
	if err != nil {
		return err
	}
	defer c.Close()
	s, err := c.NewSession()
	if err != nil {
		return err
	}
	defer s.Close()
	s.Stdout = out
	s.Stderr = out
	if stdin != nil {
		s.Stdin = stdin
	}
	done := make(chan error, 1)
	go func() { done <- s.Run(cmd) }()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case err := <-done:
		var exitMissing *ssh.ExitMissingError
		if errors.As(err, &exitMissing) || (err != nil && strings.Contains(err.Error(), "EOF")) {
			fmt.Fprintln(out, "\n(connection closed: the Steam Link is rebooting)")
			return nil
		}
		var exitErr *ssh.ExitError
		if errors.As(err, &exitErr) {
			return fmt.Errorf("the command on the Steam Link failed (exit code %d)", exitErr.ExitStatus())
		}
		return err
	}
}

type Settings struct {
	Name      string
	ServerURL string
}

// clean keeps a value safe inside a double-quoted shell assignment.
func clean(s string) string {
	return strings.Map(func(r rune) rune {
		switch r {
		case '"', '$', '`', '\\', '\n', '\r':
			return -1
		}
		return r
	}, strings.TrimSpace(s))
}

func confFile(st Settings) []byte {
	return []byte(fmt.Sprintf(`# sendspin-steamlink settings. After editing, restart the player.

# Name shown in Music Assistant
NAME="%s"

# Music Assistant's Sendspin address; the Steam Link connects to it and reconnects if it restarts.
# Leave empty to only wait for Music Assistant to connect (add the player by IP there).
SERVER_URL="%s"

# Sync tuning, in milliseconds. If this speaker plays LATE compared to your other
# speakers, raise LATENCY_MS (e.g. 50). DELAY_MS shifts playback the other way.
# Music Assistant's per-player delay setting also works and is remembered.
LATENCY_MS=0
DELAY_MS=0

# Extra options: -l debug for more logging.
EXTRA_ARGS=""
`, clean(st.Name), clean(st.ServerURL)))
}

// installBundle is the embedded device payload plus a sendspin.conf for these settings, as the
// tar.gz the device-side installer unpacks.
func installBundle(st Settings) ([]byte, error) {
	zr, err := gzip.NewReader(bytes.NewReader(payload))
	if err != nil {
		return nil, fmt.Errorf("embedded payload is damaged: %v", err)
	}
	tr := tar.NewReader(zr)
	var buf bytes.Buffer
	zw := gzip.NewWriter(&buf)
	tw := tar.NewWriter(zw)
	for {
		h, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
		if h.Typeflag != tar.TypeReg {
			continue
		}
		name := strings.TrimPrefix(h.Name, "./")
		data, err := io.ReadAll(tr)
		if err != nil {
			return nil, err
		}
		if err := writeFile(tw, name, data, 0o755); err != nil {
			return nil, err
		}
	}
	if err := writeFile(tw, "sendspin.conf", confFile(st), 0o644); err != nil {
		return nil, err
	}
	if err := tw.Close(); err != nil {
		return nil, err
	}
	if err := zw.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func writeFile(tw *tar.Writer, name string, data []byte, mode int64) error {
	// Plain ustar: the Steam Link's BusyBox tar doesn't need to understand anything newer.
	h := &tar.Header{Name: name, Mode: mode, Size: int64(len(data)), ModTime: time.Now(),
		Typeflag: tar.TypeReg, Format: tar.FormatUSTAR}
	if err := tw.WriteHeader(h); err != nil {
		return err
	}
	_, err := tw.Write(data)
	return err
}

const installCmd = `rm -rf /tmp/ss-install && mkdir -p /tmp/ss-install &&
tar xzf - -C /tmp/ss-install &&
sh /tmp/ss-install/ctl.sh install /tmp/ss-install; rc=$?
rm -rf /tmp/ss-install; exit $rc`

func install(ctx context.Context, host, password string, st Settings, out io.Writer) error {
	bundle, err := installBundle(st)
	if err != nil {
		return err
	}
	fmt.Fprintf(out, "Player name:        %s\nMusic Assistant at: %s\n\n", clean(st.Name),
		orDefault(clean(st.ServerURL), "(not set: add the player in Music Assistant by its IP)"))
	return run(ctx, host, password, installCmd, bytes.NewReader(bundle), out)
}

func deviceCommand(ctx context.Context, host, password, action string, out io.Writer) error {
	if !deviceCommands[action] {
		return fmt.Errorf("unknown action %q", action)
	}
	cmd := fmt.Sprintf("if [ -x /mnt/scratch/sendspin/ctl.sh ]; then /mnt/scratch/sendspin/ctl.sh %s; "+
		"else echo 'Sendspin is not installed on this Steam Link.'; fi", action)
	return run(ctx, host, password, cmd, nil, out)
}

func orDefault(s, d string) string {
	if s == "" {
		return d
	}
	return s
}
