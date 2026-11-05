// astrosim: bring up the engine container and the viewer, supervise both, tear down cleanly.
//
//	astrosim up [--no-display] [--host-net] [--tick N] [--verbose] ...
//	astrosim down
//	astrosim status
package main

import (
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"syscall"
	"time"

	"astrosim/orchestrator/podman"
)

func itoa(n int) string { return strconv.Itoa(n) }

func lockPath() string {
	dir := os.Getenv("XDG_RUNTIME_DIR")
	if dir == "" {
		dir = os.TempDir()
	}
	return filepath.Join(dir, "astrosim.lock")
}

// acquireLock takes an exclusive flock (released automatically if we die, even by SIGKILL) and records our pid.
func acquireLock() (*os.File, error) {
	f, err := os.OpenFile(lockPath(), os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		pid, _ := readPid(f)
		f.Close()
		return nil, fmt.Errorf("another astrosim session is running (pid %d)", pid)
	}
	_ = f.Truncate(0)
	_, _ = f.WriteAt([]byte(itoa(os.Getpid())), 0)
	return f, nil
}

func readPid(f *os.File) (int, error) {
	buf := make([]byte, 32)
	n, _ := f.ReadAt(buf, 0)
	return strconv.Atoi(string(buf[:n]))
}

// sessionPid returns the pid of a running session, or 0 if none holds the lock.
func sessionPid() int {
	f, err := os.OpenFile(lockPath(), os.O_RDWR, 0o600)
	if err != nil {
		return 0
	}
	defer f.Close()
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err == nil {
		return 0
	}
	pid, _ := readPid(f)
	return pid
}

func parseOptions(name string, args []string) (options, error) {
	fs := flag.NewFlagSet(name, flag.ContinueOnError)
	var o options
	fs.StringVar(&o.Root, "root", ".", "repository root (for the Containerfile and default viewer path)")
	fs.StringVar(&o.Image, "image", "astrosim-engine:dev", "engine image")
	fs.StringVar(&o.ViewerPath, "viewer", "", "viewer binary (default <root>/build/host/viewer)")
	fs.IntVar(&o.Port, "port", 8765, "gateway UDP port on the host")
	fs.IntVar(&o.TickHz, "tick", 0, "gateway state rate in Hz (0 = gateway default)")
	fs.BoolVar(&o.HostNet, "host-net", false, "run the container with --network=host")
	fs.BoolVar(&o.NoDisplay, "no-display", false, "engine only, no viewer")
	fs.BoolVar(&o.Verbose, "verbose", false, "stream container and viewer output")
	fs.DurationVar(&o.ReadyTimeout, "ready-timeout", 30*time.Second, "max wait for the engine to come up")
	if err := fs.Parse(args); err != nil {
		return o, err
	}
	if o.ViewerPath == "" {
		o.ViewerPath = filepath.Join(o.Root, "build", "host", "viewer")
	}
	o.Containerfile = filepath.Join(o.Root, "Containerfile")
	return o, nil
}

func preflight(o options) error {
	if err := podman.Available(); err != nil {
		return errors.New("podman not found in PATH (see SETUP.md)")
	}
	if !o.NoDisplay {
		if os.Getenv("DISPLAY") == "" && os.Getenv("WAYLAND_DISPLAY") == "" {
			return errors.New("no display (DISPLAY / WAYLAND_DISPLAY unset); use --no-display for engine only")
		}
		if st, err := os.Stat(o.ViewerPath); err != nil || st.Mode()&0o111 == 0 {
			return fmt.Errorf("viewer binary not found or not executable: %s (build it, see SETUP.md)", o.ViewerPath)
		}
	}
	pc, err := net.ListenPacket("udp", ":"+itoa(o.Port))
	if err != nil {
		return fmt.Errorf("UDP port %d is not free: %v", o.Port, err)
	}
	pc.Close()
	if !podman.ImageExists(o.Image) {
		if _, err := os.Stat(o.Containerfile); err != nil {
			return fmt.Errorf("image %s missing and no Containerfile at %s (use --root)", o.Image, o.Containerfile)
		}
		log.Printf("image %s missing; building (first build takes several minutes)", o.Image)
		if out, err := podman.Build(o.Image, o.Containerfile, o.Root); err != nil {
			return fmt.Errorf("image build failed: %v\n%s", err, out)
		}
	}
	return nil
}

func cmdUp(args []string) int {
	o, err := parseOptions("up", args)
	if err != nil {
		return 2
	}
	lock, err := acquireLock()
	if err != nil {
		log.Print(err)
		return 1
	}
	defer lock.Close()
	if err := preflight(o); err != nil {
		log.Print(err)
		return 1
	}
	if podman.Exists(containerName) {
		log.Printf("removing stale container %s", containerName)
		_ = podman.RemoveForce(containerName)
	}
	return newSession(o).run()
}

func cmdDown() int {
	pid := sessionPid()
	if pid > 0 {
		log.Printf("stopping session (pid %d)", pid)
		_ = syscall.Kill(pid, syscall.SIGTERM)
		deadline := time.Now().Add(15 * time.Second)
		for sessionPid() != 0 && time.Now().Before(deadline) {
			time.Sleep(100 * time.Millisecond)
		}
		if sessionPid() != 0 {
			log.Print("session did not exit within 15s")
			return 1
		}
	}
	if podman.Exists(containerName) {
		log.Printf("removing leftover container %s", containerName)
		_ = podman.RemoveForce(containerName)
	}
	log.Print("down")
	return 0
}

func cmdStatus() int {
	if pid := sessionPid(); pid > 0 {
		fmt.Printf("session: running (pid %d)\n", pid)
	} else {
		fmt.Println("session: not running")
	}
	switch {
	case podman.Running(containerName):
		fmt.Printf("engine container: running (%s)\n", containerName)
	case podman.Exists(containerName):
		fmt.Printf("engine container: exists but not running (%s)\n", containerName)
	default:
		fmt.Println("engine container: none")
	}
	return 0
}

func main() {
	log.SetFlags(log.Ltime)
	log.SetPrefix("astrosim: ")
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: astrosim up|down|status [flags]")
		os.Exit(2)
	}
	var code int
	switch os.Args[1] {
	case "up":
		code = cmdUp(os.Args[2:])
	case "down":
		code = cmdDown()
	case "status":
		code = cmdStatus()
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q (want up|down|status)\n", os.Args[1])
		code = 2
	}
	os.Exit(code)
}
