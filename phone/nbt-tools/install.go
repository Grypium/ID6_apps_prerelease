package main

import (
	"bytes"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// installApp copies one app folder (manifest, xml, images, lib, worker) from the
// phone into ROOT/Plugins/<name>. Safe order:
//  1. refuse if the gauge installer's lock exists or the app is already installed
//  2. write every file into ROOT/.nbt-stage-<name>-<ts>/ and read each one back
//  3. only then move the staged folder into Plugins in one rename
//
// gateway (169.254.x.x) is written as gateway-ip.txt beside the worker, like the
// official installer does. Workers (files without an extension) get mode 755.
func installApp(r runner, root, src, gateway string, log func(string, ...any)) error {
	name := filepath.Base(filepath.Clean(src))
	if err := safeName(name); err != nil || strings.ContainsAny(name, "/ ") {
		return fmt.Errorf("bad app folder name %q", name)
	}
	if ip := net.ParseIP(gateway).To4(); ip == nil || ip[0] != 169 || ip[1] != 254 {
		return errors.New("--gateway must be the car's 169.254.x.x diagnostic gateway")
	}
	if _, err := os.Stat(filepath.Join(src, name+".manifest")); err != nil {
		return fmt.Errorf("%s has no %s.manifest", src, name)
	}
	entries, err := os.ReadDir(src)
	if err != nil {
		return err
	}
	files := map[string][]byte{"gateway-ip.txt": []byte(gateway + "\n")}
	for _, e := range entries {
		if !e.Type().IsRegular() {
			return fmt.Errorf("%s: only plain files are supported", e.Name())
		}
		if err := safeName(e.Name()); err != nil {
			return err
		}
		if e.Name() == "gateway-ip.txt" {
			continue
		}
		b, err := os.ReadFile(filepath.Join(src, e.Name()))
		if err != nil {
			return err
		}
		files[e.Name()] = b
	}

	plugins := root + "/" + pluginsDir
	dest := plugins + "/" + name
	check, _ := r.run("test -e "+q(root+"/GaugeDeck-install-lock")+" && echo locked; "+
		"test -e "+q(dest)+" && echo exists; test -d "+q(plugins)+" || echo noplugins; true", nil)
	switch c := string(check); {
	case strings.Contains(c, "locked"):
		return errors.New("the gauge installer lock is present; finish or recover that install first")
	case strings.Contains(c, "exists"):
		return fmt.Errorf("%s is already installed; restore or move it aside first", name)
	case strings.Contains(c, "noplugins"):
		return fmt.Errorf("%s does not exist on the head unit", plugins)
	}

	stage := root + "/.nbt-stage-" + name + "-" + time.Now().Format("20060102-150405")
	if _, err := r.run("mkdir "+q(stage), nil); err != nil {
		return fmt.Errorf("creating staging folder: %w", err)
	}
	cleanup := func() { _, _ = r.run("rm -rf "+q(stage), nil) }
	names := make([]string, 0, len(files))
	for n := range files {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		data := files[n]
		path := stage + "/" + n
		mode := "644"
		if !strings.Contains(n, ".") {
			mode = "755" // worker executable
		}
		if _, err := r.run("cat > "+q(path)+" && chmod "+mode+" "+q(path), data); err != nil {
			cleanup()
			return fmt.Errorf("writing %s: %w", n, err)
		}
		back, err := r.run("cat "+q(path), nil)
		if err != nil || !bytes.Equal(back, data) {
			cleanup()
			return fmt.Errorf("read-back check failed for %s; nothing was installed", n)
		}
		log("  staged %s (%d bytes, mode %s, verified)", n, len(data), mode)
	}
	if _, err := r.run("test ! -e "+q(dest)+" && mv "+q(stage)+" "+q(dest), nil); err != nil {
		cleanup()
		return fmt.Errorf("moving %s into Plugins: %w", name, err)
	}
	log("Installed %s at %s", name, dest)
	return nil
}
