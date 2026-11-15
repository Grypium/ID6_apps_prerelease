// nbt-backup: back up, verify and restore the BMW NBT Evo head unit's HMI
// Plugins folder over SSH from the phone. Restore never deletes anything on
// the car: items not in the backup are moved into ID5/Removed-<timestamp>/.
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const usage = `nbt-backup — back up and restore the head unit's app (Plugins) folder

  nbt-backup backup    --head IP              save a full copy to the phone
  nbt-backup list                             show saved backups
  nbt-backup verify    --head IP [--from DIR] compare the car to a backup (read-only)
  nbt-backup restore   --head IP [--from DIR] [--yes]
                                              make the car match a backup
  nbt-backup pull-libs --head IP              copy QNX runtime + framework libraries
                                              to the phone (for building apps)
  nbt-backup install-app --head IP --app DIR --gateway GW
                                              add one app folder to Plugins (staged,
                                              read back, never overwrites)

--from defaults to the FACTORY backup (the oldest one taken with no gauge apps installed).
restore without --yes only shows what it would do.
Backups are stored in ~/nbt-backups (override with --dir).
After a restore, hold the iDrive volume/power knob ~30 s while parked to restart it.
`

func main() {
	if len(os.Args) < 2 {
		fmt.Print(usage)
		os.Exit(2)
	}
	cmd := os.Args[1]
	fs := flag.NewFlagSet(cmd, flag.ExitOnError)
	head := fs.String("head", "", "head unit SSH IP (169.254.x.x)")
	from := fs.String("from", "", "backup folder to verify/restore against")
	yes := fs.Bool("yes", false, "apply the restore")
	home, _ := os.UserHomeDir()
	dir := fs.String("dir", filepath.Join(home, "nbt-backups"), "where backups are kept")
	root := fs.String("root", defaultID5Root, "ID5 folder on the head unit (advanced)")
	via := fs.String("via", "", "dial this local relay instead of HEAD:22 (Android: ~/bin/eth-fwd)")
	app := fs.String("app", "", "install-app: local app folder (e.g. FaultCodes/)")
	gateway := fs.String("gateway", "", "install-app: car diagnostic gateway IP for gateway-ip.txt")
	anyHost := fs.Bool("any-host", false, "allow a non-169.254 IP (testing only)")
	_ = fs.Parse(os.Args[2:])
	_ = os.MkdirAll(*dir, 0o700)
	logf := func(f string, a ...any) { fmt.Printf(f+"\n", a...) }

	if cmd == "list" {
		for _, b := range backups(*dir) {
			fmt.Println(b)
		}
		return
	}
	if cmd != "backup" && cmd != "verify" && cmd != "restore" && cmd != "pull-libs" && cmd != "install-app" {
		fmt.Print(usage)
		os.Exit(2)
	}
	if *head == "" {
		fail("--head IP is required")
	}
	var snap *snapshot
	var snapDir string
	if cmd == "verify" || cmd == "restore" {
		snapDir = *from
		if snapDir == "" {
			snapDir = factory(*dir)
			if snapDir == "" {
				fail("no factory backup found in " + *dir + "; run 'nbt-backup backup' before installing anything")
			}
		}
		var err error
		if snap, err = loadSnapshot(snapDir); err != nil {
			fail(err.Error())
		}
		fmt.Printf("Using backup %s (taken %s, %s)\n", snapDir, snap.Created, firmwareOf(snap))
	}

	r, err := connect(*head, *via, *anyHost, *dir)
	if err != nil {
		fail(err.Error())
	}
	defer r.close()

	switch cmd {
	case "backup":
		name := time.Now().Format("20060102-150405")
		out := filepath.Join(*dir, name)
		fmt.Println("Backing up", *root+"/Plugins", "to", out)
		s, err := takeBackup(r, *root, *head, loadKnown(*dir)[*head], out, logf)
		if err != nil {
			fail("backup failed: " + err.Error())
		}
		n := 0
		for _, e := range s.Entries {
			if e.Kind == "file" {
				n++
			}
		}
		fmt.Printf("Backup complete and self-checked: %d files, %s\n", n, firmwareOf(s))
		if s.HasGauges {
			fmt.Println("NOTE: gauge apps were already installed, so this is NOT a factory backup.")
		} else {
			fmt.Println("No gauge apps present: this backup counts as a FACTORY backup.")
		}
		if s.Head != "" && snapshotForeign(*dir, s) {
			fmt.Println("WARNING: framework hashes differ from your earlier backups. Is this the same car/firmware?")
		}
	case "verify":
		if snap.Fingerprint != "" && loadKnown(*dir)[*head] != snap.Fingerprint {
			fmt.Println("WARNING: this head unit's SSH key differs from the one in the backup.")
		}
		d, err := compare(r, snap)
		if err != nil {
			fail(err.Error())
		}
		fmt.Println(describe(d))
	case "restore":
		if snap.Fingerprint != "" && loadKnown(*dir)[*head] != snap.Fingerprint {
			fail("this head unit's SSH key differs from the one in the backup; refusing to restore onto a different unit")
		}
		d, err := compare(r, snap)
		if err != nil {
			fail(err.Error())
		}
		fmt.Println(describe(d))
		if d.empty() {
			return
		}
		if !*yes {
			fmt.Println("\nDry run only. Re-run with --yes to apply.")
			return
		}
		if err := restore(r, snap, snapDir, logf); err != nil {
			fail("restore stopped: " + err.Error())
		}
		fmt.Println("Done. While parked, hold the volume/power knob ~30 s to restart iDrive.")
	case "install-app":
		if *app == "" || *gateway == "" {
			fail("install-app needs --app DIR and --gateway IP")
		}
		if err := installApp(r, *root, *app, *gateway, logf); err != nil {
			fail("install stopped: " + err.Error())
		}
		fmt.Println("Done. While parked, hold the volume/power knob ~30 s to restart iDrive.")
		fmt.Println("Undo: nbt-backup restore (moves added apps aside, never deletes).")
	case "pull-libs":
		pullLibs(r, filepath.Join(*dir, "libs"))
	}
}

func fail(msg string) {
	fmt.Fprintln(os.Stderr, "error:", msg)
	os.Exit(1)
}

func firmwareOf(s *snapshot) string {
	if f := firmwareName(s.Firmware); f != "" {
		return f
	}
	return "firmware unknown"
}

func backups(dir string) []string {
	ents, _ := os.ReadDir(dir)
	var out []string
	for _, e := range ents {
		p := filepath.Join(dir, e.Name())
		s, err := loadSnapshot(p)
		if err != nil {
			continue
		}
		tag := "factory"
		if s.HasGauges {
			tag = "has gauge apps"
		}
		out = append(out, fmt.Sprintf("%s  %s  %d entries  [%s]", p, firmwareOf(s), len(s.Entries), tag))
	}
	sort.Strings(out)
	return out
}

// factory returns the oldest valid backup taken without the gauge apps installed.
func factory(dir string) string {
	ents, _ := os.ReadDir(dir)
	var names []string
	for _, e := range ents {
		if e.IsDir() {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	for _, n := range names {
		p := filepath.Join(dir, n)
		if s, err := loadSnapshot(p); err == nil && !s.HasGauges {
			return p
		}
	}
	return ""
}

func snapshotForeign(dir string, s *snapshot) bool {
	f := factory(dir)
	if f == "" {
		return false
	}
	old, err := loadSnapshot(f)
	return err == nil && len(old.Frameworks) > 0 && strings.Join(old.Frameworks, ",") != strings.Join(s.Frameworks, ",")
}

func firmwareName(text string) string {
	i := strings.Index(strings.ToUpper(text), "NBTEVO_")
	if i < 0 || len(text) < i+13 {
		return ""
	}
	return "NBTevo_" + strings.ToUpper(text[i+7:i+13])
}

// pullLibs copies the QNX runtime libraries and HMI frameworks the apps link
// against, for building on the phone. These stay private on the phone.
func pullLibs(r runner, out string) {
	_ = os.MkdirAll(out, 0o700)
	names := []string{"libc.so.3", "libm.so.2", "libsocket.so.3", "libstdc++.so.6", "ldqnx.so.2",
		"libAppFramework.A.so", "libCoreFramework.A.so"}
	dirs := "/opt/hmi/ID5/lib /lib /usr/lib /proc/boot /lib/dll /usr/qnx650/target/qnx6/armle-v7/lib"
	for _, n := range names {
		where, _ := r.run("for d in "+dirs+"; do if test -f \"$d/"+n+"\"; then echo \"$d/"+n+"\"; break; fi; done", nil)
		p := strings.TrimSpace(string(where))
		if p == "" {
			fmt.Println("  not found:", n)
			continue
		}
		data, err := r.run("cat "+q(p), nil)
		if err != nil {
			fmt.Println("  could not copy", p, err)
			continue
		}
		_ = os.WriteFile(filepath.Join(out, n), data, 0o600)
		fmt.Printf("  %s (%d bytes) sha256 %s\n", p, len(data), sum(data))
	}
	fmt.Println("Saved to", out)
}
