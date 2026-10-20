// Steam Link Sendspin: turns an original Valve Steam Link into a Sendspin speaker for Music
// Assistant. Opens a small page in the browser (served only to this computer) that finds the
// Steam Link, installs the player over SSH and manages it.
package main

import (
	"context"
	"crypto/rand"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"sync"
	"sync/atomic"
	"time"
)

//go:embed payload.tar.gz
var payload []byte

//go:embed web/index.html
var indexHTML []byte

var buildInfo = "dev" // set with -ldflags "-X main.buildInfo=..."

type app struct {
	token    string
	lastSeen atomic.Int64 // unix seconds of the page's last heartbeat
	busy     atomic.Int32 // operations in progress
	quit     chan struct{}
	quitOnce sync.Once
}

func main() {
	noBrowser := flag.Bool("no-browser", false, "don't open the browser, just print the address")
	port := flag.Int("port", 47823, "port for the local page")
	flag.Parse()

	tok := make([]byte, 16)
	rand.Read(tok)
	a := &app{token: hex.EncodeToString(tok), quit: make(chan struct{})}
	a.lastSeen.Store(time.Now().Unix())

	// A fixed port keeps the page's address the same between runs, so the browser remembers the
	// Steam Link's address; any free port if it's taken.
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", *port))
	if err != nil {
		ln, err = net.Listen("tcp", "127.0.0.1:0")
	}
	if err != nil {
		fatal("couldn't start the local page: %v", err)
	}
	url := fmt.Sprintf("http://%s/?t=%s", ln.Addr(), a.token)

	mux := http.NewServeMux()
	mux.HandleFunc("/", a.page)
	mux.HandleFunc("/api/info", a.guard(a.info))
	mux.HandleFunc("/api/devices", a.guard(a.devices))
	mux.HandleFunc("/api/servers", a.guard(a.servers))
	mux.HandleFunc("/api/run", a.guard(a.runAction))
	mux.HandleFunc("/api/ping", a.guard(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(204) }))
	mux.HandleFunc("/api/quit", a.guard(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(204)
		a.stop()
	}))
	srv := &http.Server{Handler: mux}
	go srv.Serve(ln)

	fmt.Printf("Steam Link Sendspin is running at %s\nClose the browser tab (or press Ctrl+C) to quit.\n", url)
	if !*noBrowser {
		openBrowser(url)
	}

	// Quit when the page has been gone for a while (tab closed) and nothing is running.
	go func() {
		for range time.Tick(5 * time.Second) {
			if a.busy.Load() == 0 && time.Now().Unix()-a.lastSeen.Load() > 45 {
				a.stop()
				return
			}
		}
	}()
	<-a.quit
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	srv.Shutdown(ctx)
}

func (a *app) stop() { a.quitOnce.Do(func() { close(a.quit) }) }

// guard admits only requests from our own page: they carry the per-run token, which other
// websites open in the same browser can't know.
func (a *app) guard(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Token") != a.token {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		a.lastSeen.Store(time.Now().Unix())
		h(w, r)
	}
}

func (a *app) page(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" || r.URL.Query().Get("t") != a.token {
		http.Error(w, "Open the address shown by Steam Link Sendspin.", http.StatusForbidden)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.Write(indexHTML)
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}

func (a *app) info(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, map[string]string{"build": buildInfo})
}

func (a *app) devices(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
	defer cancel()
	writeJSON(w, findDevices(ctx))
}

func (a *app) servers(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 4*time.Second)
	defer cancel()
	writeJSON(w, findServers(ctx))
}

type runRequest struct {
	Action   string `json:"action"`
	Host     string `json:"host"`
	Password string `json:"password"`
	Name     string `json:"name"`
	Server   string `json:"server"`
}

// flushWriter sends each write to the browser straight away.
type flushWriter struct {
	w http.ResponseWriter
	f http.Flusher
}

func (fw flushWriter) Write(p []byte) (int, error) {
	n, err := fw.w.Write(p)
	fw.f.Flush()
	return n, err
}

func (a *app) runAction(w http.ResponseWriter, r *http.Request) {
	var req runRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Host == "" {
		http.Error(w, "missing details", http.StatusBadRequest)
		return
	}
	if net.ParseIP(req.Host) == nil {
		if _, err := net.LookupHost(req.Host); err != nil {
			http.Error(w, fmt.Sprintf("%q isn't an address on this network", req.Host), http.StatusBadRequest)
			return
		}
	}
	a.busy.Add(1)
	defer a.busy.Add(-1)

	f, _ := w.(http.Flusher)
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.WriteHeader(200)
	out := flushWriter{w, f}

	ctx, cancel := context.WithTimeout(r.Context(), 3*time.Minute)
	defer cancel()
	var err error
	if req.Action == "install" {
		err = install(ctx, req.Host, req.Password,
			Settings{Name: req.Name, ServerURL: normalizeServer(req.Server)}, out)
	} else {
		err = deviceCommand(ctx, req.Host, req.Password, req.Action, out)
	}
	// Last line tells the page how it ended.
	if err != nil {
		fmt.Fprintf(out, "\n\x1eERROR %s\n", err)
	} else {
		fmt.Fprintf(out, "\n\x1eOK\n")
	}
}

func openBrowser(url string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	case "darwin":
		cmd = exec.Command("open", url)
	default:
		cmd = exec.Command("xdg-open", url)
	}
	if err := cmd.Start(); err != nil {
		log.Printf("Open this address in your browser: %s", url)
	}
}

func fatal(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Fprintln(os.Stderr, msg)
	showError(msg)
	os.Exit(1)
}
