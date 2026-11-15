// Package hulink opens a root SSH connection to the head unit for the small
// helper commands (hu, hu-get). The host key must match the fingerprint that
// nbt-backup saved in ~/nbt-backups/known_hosts on first contact.
//
// Environment:
//
//	NBT_PW    root password (required)
//	HU_HEAD   head unit IP as saved in known_hosts (default: the only entry)
//	HU_ADDR   address to dial (default 127.0.0.1:2222, the eth-fwd relay)
package hulink

import (
	"bufio"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"

	"golang.org/x/crypto/ssh"
)

func pinned() (head, fp string, err error) {
	home, _ := os.UserHomeDir()
	f, err := os.Open(filepath.Join(home, "nbt-backups", "known_hosts"))
	if err != nil {
		return "", "", errors.New("no ~/nbt-backups/known_hosts: run 'nbt-backup backup' once to pin the head unit's key")
	}
	defer f.Close()
	want := os.Getenv("HU_HEAD")
	known := map[string]string{}
	s := bufio.NewScanner(f)
	for s.Scan() {
		if p := strings.Fields(s.Text()); len(p) == 2 {
			known[p[0]] = p[1]
		}
	}
	if want != "" {
		if fp, ok := known[want]; ok {
			return want, fp, nil
		}
		return "", "", fmt.Errorf("%s is not in known_hosts", want)
	}
	if len(known) != 1 {
		return "", "", errors.New("set HU_HEAD: known_hosts has several head units")
	}
	for h, fp := range known {
		return h, fp, nil
	}
	return "", "", errors.New("known_hosts is empty")
}

// Dial connects and authenticates; the caller closes the client.
func Dial() (*ssh.Client, error) {
	head, fp, err := pinned()
	if err != nil {
		return nil, err
	}
	pw := os.Getenv("NBT_PW")
	if pw == "" {
		return nil, errors.New("set NBT_PW to the head unit's root password")
	}
	addr := os.Getenv("HU_ADDR")
	if addr == "" {
		addr = "127.0.0.1:2222"
	}
	c, err := net.DialTimeout("tcp4", addr, 6*time.Second)
	if err != nil {
		return nil, fmt.Errorf("dial %s (is eth-fwd running?): %w", addr, err)
	}
	cfg := &ssh.ClientConfig{
		User: "root",
		Auth: []ssh.AuthMethod{ssh.Password(pw)},
		HostKeyCallback: func(_ string, _ net.Addr, k ssh.PublicKey) error {
			if got := ssh.FingerprintSHA256(k); got != fp {
				return fmt.Errorf("host key mismatch for %s: got %s", head, got)
			}
			return nil
		},
		HostKeyAlgorithms: []string{"ssh-rsa", "ssh-dss"},
		Timeout:           15 * time.Second,
	}
	cfg.Config.KeyExchanges = []string{"diffie-hellman-group14-sha1", "diffie-hellman-group1-sha1"}
	cfg.Config.Ciphers = []string{"aes128-ctr", "aes256-ctr", "aes128-cbc", "3des-cbc"}
	sc, ch, rq, err := ssh.NewClientConn(c, head+":22", cfg)
	if err != nil {
		c.Close()
		return nil, err
	}
	return ssh.NewClient(sc, ch, rq), nil
}
