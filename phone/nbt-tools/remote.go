package main

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"golang.org/x/crypto/ssh"
	"golang.org/x/term"
)

// runner executes a shell command on the head unit (or, in tests, locally).
type runner interface {
	run(cmd string, stdin []byte) ([]byte, error)
}

type sshRunner struct {
	client *ssh.Client
	sock   net.Conn
}

func (r *sshRunner) run(cmd string, stdin []byte) ([]byte, error) {
	_ = r.sock.SetDeadline(time.Now().Add(60 * time.Second))
	s, err := r.client.NewSession()
	if err != nil {
		return nil, err
	}
	defer s.Close()
	var out, errb bytes.Buffer
	s.Stdin = bytes.NewReader(stdin)
	s.Stdout, s.Stderr = &out, &errb
	if err := s.Run(cmd); err != nil {
		if d := strings.TrimSpace(errb.String()); d != "" {
			return nil, fmt.Errorf("%w: %s", err, d)
		}
		return nil, err
	}
	return out.Bytes(), nil
}

func (r *sshRunner) close() { r.client.Close() }

// localRunner runs commands with the local /bin/sh. Used by tests.
type localRunner struct{}

func (localRunner) run(cmd string, stdin []byte) ([]byte, error) {
	c := exec.Command("sh", "-c", cmd)
	c.Stdin = bytes.NewReader(stdin)
	var out, errb bytes.Buffer
	c.Stdout, c.Stderr = &out, &errb
	if err := c.Run(); err != nil {
		if d := strings.TrimSpace(errb.String()); d != "" {
			return nil, fmt.Errorf("%w: %s", err, d)
		}
		return nil, err
	}
	return out.Bytes(), nil
}

func linkLocal(ip string) bool {
	a, err := netip.ParseAddr(ip)
	return err == nil && a.Is4() && a.IsLinkLocalUnicast()
}

// knownHosts maps head-unit IP -> SHA256 host key fingerprint, stored beside the backups.
func knownHostsPath(base string) string { return filepath.Join(base, "known_hosts") }

func loadKnown(base string) map[string]string {
	m := map[string]string{}
	f, err := os.Open(knownHostsPath(base))
	if err != nil {
		return m
	}
	defer f.Close()
	s := bufio.NewScanner(f)
	for s.Scan() {
		if parts := strings.Fields(s.Text()); len(parts) == 2 {
			m[parts[0]] = parts[1]
		}
	}
	return m
}

func saveKnown(base, host, fp string) error {
	m := loadKnown(base)
	m[host] = fp
	var b strings.Builder
	for h, f := range m {
		fmt.Fprintf(&b, "%s %s\n", h, f)
	}
	return os.WriteFile(knownHostsPath(base), []byte(b.String()), 0o600)
}

func prompt(msg string) string {
	fmt.Fprint(os.Stderr, msg)
	line, _ := bufio.NewReader(os.Stdin).ReadString('\n')
	return strings.TrimSpace(line)
}

// connect opens SSH to the head unit as root. The host key must match the saved
// fingerprint, or the user must confirm it on first contact.
func connect(host, via string, anyHost bool, base string) (*sshRunner, error) {
	if !anyHost && !linkLocal(host) {
		return nil, errors.New("head unit IP must be an ENET 169.254.x.x address")
	}
	known := loadKnown(base)[host]
	var seen string
	trust := func(fp string) error {
		seen = fp
		if known != "" {
			if fp != known {
				return fmt.Errorf("HOST KEY CHANGED for %s (saved %s, got %s). Re-check the IP belongs to your head unit", host, known, fp)
			}
			return nil
		}
		fmt.Fprintf(os.Stderr, "First connection to %s.\nHost key fingerprint: %s\n", host, fp)
		if prompt("Type yes if this is your head unit: ") != "yes" {
			return errors.New("fingerprint not confirmed")
		}
		known = fp
		return nil
	}
	addr := net.JoinHostPort(host, "22")
	if via != "" { // local relay (e.g. ~/bin/eth-fwd) that reaches host:22 over Ethernet
		addr = via
	}
	sock, err := net.DialTimeout("tcp4", addr, 6*time.Second)
	if err != nil {
		return nil, fmt.Errorf("could not reach SSH on %s (is SSH enabled?): %w", addr, err)
	}
	var pw []byte
	if env := os.Getenv("NBT_PW"); env != "" {
		pw = []byte(env)
	} else {
		fmt.Fprint(os.Stderr, "Head unit root password (not shown): ")
		pw, err = term.ReadPassword(int(os.Stdin.Fd()))
		fmt.Fprintln(os.Stderr)
	}
	if err != nil {
		sock.Close()
		return nil, err
	}
	cfg := &ssh.ClientConfig{
		User: "root",
		Config: ssh.Config{
			KeyExchanges: append(append([]string{}, ssh.SupportedAlgorithms().KeyExchanges...),
				"diffie-hellman-group14-sha1", "diffie-hellman-group1-sha1"),
			Ciphers: append(append([]string{}, ssh.SupportedAlgorithms().Ciphers...), "aes128-cbc", "aes256-cbc", "3des-cbc"),
			MACs:    append(append([]string{}, ssh.SupportedAlgorithms().MACs...), "hmac-sha1", "hmac-sha1-96"),
		},
		HostKeyAlgorithms: append(append([]string{}, ssh.SupportedAlgorithms().HostKeys...), "ssh-rsa", "ssh-dss"),
		Auth: []ssh.AuthMethod{ssh.Password(string(pw)), ssh.KeyboardInteractive(
			func(_, _ string, qs []string, _ []bool) ([]string, error) {
				a := make([]string, len(qs))
				for i := range a {
					a[i] = string(pw)
				}
				return a, nil
			})},
		HostKeyCallback: func(_ string, _ net.Addr, k ssh.PublicKey) error { return trust(ssh.FingerprintSHA256(k)) },
		Timeout:         8 * time.Second,
	}
	_ = sock.SetDeadline(time.Now().Add(15 * time.Second))
	c, ch, rq, err := ssh.NewClientConn(sock, addr, cfg)
	if err != nil {
		sock.Close()
		return nil, err
	}
	if err := saveKnown(base, host, seen); err != nil {
		fmt.Fprintln(os.Stderr, "warning: could not save fingerprint:", err)
	}
	return &sshRunner{ssh.NewClient(c, ch, rq), sock}, nil
}
