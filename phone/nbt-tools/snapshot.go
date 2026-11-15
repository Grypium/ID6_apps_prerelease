package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Default head-unit locations. The gauge installer only writes under ID5Root:
// the Plugins folder, an install lock, and GaugeDeck-* staging folders.
const (
	defaultID5Root = "/var/opt/hmi/data/ID5"
	pluginsDir     = "Plugins"
)

type entry struct {
	Path   string `json:"path"` // relative to Plugins, "/"-separated, no leading "./"
	Kind   string `json:"kind"` // "dir" | "file" | "link"
	Mode   string `json:"mode"` // octal permission bits, e.g. "755"
	Size   int    `json:"size,omitempty"`
	SHA256 string `json:"sha256,omitempty"`
	Target string `json:"target,omitempty"` // symlink target (recorded, not restored automatically)
}

type snapshot struct {
	Created     string   `json:"created"`
	Head        string   `json:"head"`
	Fingerprint string   `json:"fingerprint,omitempty"`
	Root        string   `json:"root"`
	Firmware    string   `json:"firmware"`
	Uname       string   `json:"uname"`
	Frameworks  []string `json:"frameworks"`                  // sha256 of the two HMI framework libraries
	HasGauges   bool     `json:"has_gauge_apps"`              // BoostDeck/ShiftDeck/MhdMaps were present at backup time
	NoPlugins   bool     `json:"no_plugins_folder,omitempty"` // stock unit: Plugins did not exist yet
	Entries     []entry  `json:"entries"`
}

var gaugeApps = []string{"BoostDeck", "ShiftDeck", "MhdMaps"}

func q(s string) string { return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'" }

func safeName(p string) error {
	if p == "" || strings.ContainsAny(p, "\n\r'\x00") || strings.HasPrefix(p, "/") {
		return fmt.Errorf("unsupported file name %q", p)
	}
	for _, part := range strings.Split(p, "/") {
		if part == ".." || part == "." || part == "" {
			return fmt.Errorf("unsupported file name %q", p)
		}
	}
	return nil
}

// modeOf converts an ls permission string like "-rwxr-xr-x" to "755".
// Setuid/sticky bits are not expected under Plugins and are recorded as plain bits.
func modeOf(perm string) (string, error) {
	if len(perm) < 10 {
		return "", fmt.Errorf("unexpected ls output %q", perm)
	}
	v := 0
	for i, c := range perm[1:10] {
		if c != '-' && c != 'S' && c != 'T' {
			v |= 1 << (8 - i)
		}
	}
	return strconv.FormatInt(int64(v), 8), nil
}

const (
	noPluginsMark = "__NBT_NO_PLUGINS__"
	countMark     = "__NBT_COUNT__"
)

// list enumerates everything under ROOT/Plugins with kinds and permissions.
// exists is false when the Plugins folder is absent (stock unit, nothing installed).
func list(r runner, root string) (es []entry, exists bool, err error) {
	base := root + "/" + pluginsDir
	// QNX ksh has no printf; use the print builtin when present (local tests run bash).
	script := `test -e ` + q(base) + ` || { echo ` + noPluginsMark + `; exit 0; }
cd ` + q(base) + ` || exit 1
if (print -r -- x) >/dev/null 2>&1; then out() { print -r -- "$1"; }; else out() { echo "$1"; }; fi
for t in d f l; do find . -type $t | while IFS= read -r p; do
  test "$p" = . && continue
  out "$t$p"; out "$(ls -ldn "$p")"
  if test $t = l; then out "$(ls -l "$p" | sed 's/.* -> //')"; fi
done; done
echo ` + countMark + `
find . | while IFS= read -r p; do echo x; done`
	out, err := r.run(script, nil)
	if err != nil {
		return nil, false, fmt.Errorf("listing %s failed: %w", base, err)
	}
	if strings.TrimSpace(string(out)) == noPluginsMark {
		return nil, false, nil
	}
	body, counted, ok := strings.Cut(string(out), countMark+"\n")
	if !ok {
		return nil, true, fmt.Errorf("listing %s was incomplete (no count marker)", base)
	}
	total := strings.Count(counted, "x\n") - 1 // minus "." itself
	lines := strings.Split(strings.TrimRight(body, "\n"), "\n")
	if len(lines) == 1 && lines[0] == "" {
		lines = nil
	}
	for i := 0; i+1 < len(lines); {
		head, ls := lines[i], lines[i+1]
		i += 2
		if len(head) < 3 || !strings.HasPrefix(head[1:], "./") {
			return nil, true, fmt.Errorf("unexpected listing line %q", head)
		}
		e := entry{Path: head[3:]}
		if err := safeName(e.Path); err != nil {
			return nil, true, err
		}
		mode, err := modeOf(strings.Fields(ls + " x")[0])
		if err != nil {
			return nil, true, err
		}
		e.Mode = mode
		switch head[0] {
		case 'd':
			e.Kind = "dir"
		case 'f':
			e.Kind = "file"
		case 'l':
			e.Kind = "link"
			if i < len(lines) {
				e.Target = lines[i]
				i++
			}
		}
		es = append(es, e)
	}
	if len(es) != total {
		return nil, true, fmt.Errorf("listing %s found %d items but the folder has %d; refusing to continue", base, len(es), total)
	}
	sort.Slice(es, func(a, b int) bool { return es[a].Path < es[b].Path })
	return es, true, nil
}

func fetch(r runner, root, rel string) ([]byte, error) {
	return r.run("cat "+q(root+"/"+pluginsDir+"/"+rel), nil)
}

func sum(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }

func firstLine(b []byte) string { return strings.TrimSpace(strings.SplitN(string(b), "\n", 2)[0]) }

// takeBackup copies ROOT/Plugins into dir/files and writes dir/snapshot.json.
func takeBackup(r runner, root, head, fp, dir string, log func(string, ...any)) (*snapshot, error) {
	s := &snapshot{Created: time.Now().Format(time.RFC3339), Head: head, Fingerprint: fp, Root: root}
	if b, err := r.run("cat /opt/sys/etc/nbt_version.txt 2>/dev/null; true", nil); err == nil {
		s.Firmware = strings.TrimSpace(string(b))
	}
	if b, err := r.run("uname -a", nil); err == nil {
		s.Uname = firstLine(b)
	}
	for _, lib := range []string{"libAppFramework.A.so", "libCoreFramework.A.so"} {
		if b, err := r.run("cat /opt/hmi/ID5/lib/"+lib, nil); err == nil && len(b) > 0 {
			s.Frameworks = append(s.Frameworks, lib+" "+sum(b))
		}
	}
	if lock, _ := r.run("test -e "+q(root+"/GaugeDeck-install-lock")+" && echo locked; true", nil); strings.TrimSpace(string(lock)) == "locked" {
		log("WARNING: a gauge installer lock is present; an install may be unfinished.")
	}
	es, exists, err := list(r, root)
	if err != nil {
		return nil, err
	}
	if !exists {
		s.NoPlugins = true
		log("No Plugins folder on the head unit (stock: nothing installed). Recorded as factory state.")
	}
	if err := os.MkdirAll(filepath.Join(dir, "files"), 0o700); err != nil {
		return nil, err
	}
	for i := range es {
		e := &es[i]
		if !strings.Contains(e.Path, "/") {
			for _, g := range gaugeApps {
				if e.Path == g {
					s.HasGauges = true
				}
			}
		}
		if e.Kind != "file" {
			continue
		}
		data, err := fetch(r, root, e.Path)
		if err != nil {
			return nil, fmt.Errorf("copying %s: %w", e.Path, err)
		}
		e.Size, e.SHA256 = len(data), sum(data)
		local := filepath.Join(dir, "files", filepath.FromSlash(e.Path))
		if err := os.MkdirAll(filepath.Dir(local), 0o700); err != nil {
			return nil, err
		}
		if err := os.WriteFile(local, data, 0o600); err != nil {
			return nil, err
		}
		log("  saved %s (%d bytes)", e.Path, e.Size)
	}
	s.Entries = es
	if listing, err := r.run("ls -lR "+q(root), nil); err == nil {
		_ = os.WriteFile(filepath.Join(dir, "id5-listing.txt"), listing, 0o600)
	}
	js, _ := json.MarshalIndent(s, "", "  ")
	if err := os.WriteFile(filepath.Join(dir, "snapshot.json"), js, 0o600); err != nil {
		return nil, err
	}
	// Re-read every saved file to make sure the backup on the phone is intact.
	if bad := checkLocal(dir, s); len(bad) > 0 {
		return nil, fmt.Errorf("backup self-check failed: %s", strings.Join(bad, ", "))
	}
	return s, nil
}

func loadSnapshot(dir string) (*snapshot, error) {
	b, err := os.ReadFile(filepath.Join(dir, "snapshot.json"))
	if err != nil {
		return nil, err
	}
	var s snapshot
	if err := json.Unmarshal(b, &s); err != nil {
		return nil, err
	}
	for _, e := range s.Entries {
		if err := safeName(e.Path); err != nil {
			return nil, err
		}
	}
	if bad := checkLocal(dir, &s); len(bad) > 0 {
		return nil, fmt.Errorf("backup on phone is damaged: %s", strings.Join(bad, ", "))
	}
	return &s, nil
}

func checkLocal(dir string, s *snapshot) []string {
	var bad []string
	for _, e := range s.Entries {
		if e.Kind != "file" {
			continue
		}
		b, err := os.ReadFile(filepath.Join(dir, "files", filepath.FromSlash(e.Path)))
		if err != nil || sum(b) != e.SHA256 {
			bad = append(bad, e.Path)
		}
	}
	return bad
}

type diff struct {
	WholeFolder bool // backup had no Plugins folder, but one exists now
	folderItems int
	Extra       []entry // on the unit, not in the backup
	Missing     []entry // in the backup, not on the unit (or wrong kind)
	Changed     []entry // file content differs
	ModeOnly    []entry // same content, different permissions
}

func (d diff) empty() bool {
	return !d.WholeFolder && len(d.Extra)+len(d.Missing)+len(d.Changed)+len(d.ModeOnly) == 0
}

// compare checks the live Plugins tree against a snapshot. File contents are
// read back and hashed on the phone, since QNX may lack sha256sum.
func compare(r runner, s *snapshot) (diff, error) {
	live, exists, err := list(r, s.Root)
	if err != nil {
		return diff{}, err
	}
	if s.NoPlugins {
		// Factory state is "no Plugins folder at all".
		return diff{WholeFolder: exists, folderItems: len(live)}, nil
	}
	want := map[string]entry{}
	for _, e := range s.Entries {
		want[e.Path] = e
	}
	have := map[string]entry{}
	for _, e := range live {
		have[e.Path] = e
	}
	var d diff
	for _, e := range live {
		w, ok := want[e.Path]
		if !ok || w.Kind != e.Kind {
			// Only report the top-most extra path; its contents move with it.
			if parent := path.Dir(e.Path); parent != "." {
				if pe, pok := have[parent]; pok {
					if wp, wok := want[parent]; !wok || wp.Kind != pe.Kind {
						continue
					}
				}
			}
			d.Extra = append(d.Extra, e)
		}
	}
	for _, w := range s.Entries {
		e, ok := have[w.Path]
		if !ok || e.Kind != w.Kind {
			d.Missing = append(d.Missing, w)
			continue
		}
		if w.Kind == "file" {
			data, err := fetch(r, s.Root, w.Path)
			if err != nil {
				return diff{}, err
			}
			if sum(data) != w.SHA256 {
				d.Changed = append(d.Changed, w)
				continue
			}
		}
		if w.Kind != "link" && e.Mode != w.Mode {
			d.ModeOnly = append(d.ModeOnly, w)
		}
	}
	return d, nil
}

func describe(d diff) string {
	if d.empty() {
		return "Plugins folder matches the backup exactly."
	}
	if d.WholeFolder {
		return fmt.Sprintf("  extra    Plugins folder (%d items; did not exist at backup time) -> whole folder will be moved aside, not deleted", d.folderItems)
	}
	var b strings.Builder
	for _, e := range d.Extra {
		fmt.Fprintf(&b, "  extra    %s (%s) -> will be moved aside, not deleted\n", e.Path, e.Kind)
	}
	for _, e := range d.Missing {
		fmt.Fprintf(&b, "  missing  %s (%s) -> will be restored\n", e.Path, e.Kind)
	}
	for _, e := range d.Changed {
		fmt.Fprintf(&b, "  changed  %s -> original will be written back\n", e.Path)
	}
	for _, e := range d.ModeOnly {
		fmt.Fprintf(&b, "  perms    %s -> chmod %s\n", e.Path, e.Mode)
	}
	return strings.TrimRight(b.String(), "\n")
}

// restore makes ROOT/Plugins match the snapshot. Extra items are moved into
// ROOT/Removed-<timestamp>/ rather than deleted. Every written file is read back.
func restore(r runner, s *snapshot, dir string, log func(string, ...any)) error {
	if lock, _ := r.run("test -e "+q(s.Root+"/GaugeDeck-install-lock")+" && echo locked; true", nil); strings.TrimSpace(string(lock)) == "locked" {
		return errors.New("the gauge installer lock is present; finish or recover that install first (installer 'Clear lock and install' / recover)")
	}
	d, err := compare(r, s)
	if err != nil {
		return err
	}
	if d.empty() {
		log("Nothing to do: Plugins already matches the backup.")
		return nil
	}
	base := s.Root + "/" + pluginsDir
	if d.WholeFolder {
		aside := s.Root + "/Removed-" + time.Now().Format("20060102-150405")
		if _, err := r.run("mkdir "+q(aside)+" && mv "+q(base)+" "+q(aside+"/"+pluginsDir), nil); err != nil {
			return fmt.Errorf("moving Plugins aside to %s: %w", aside, err)
		}
		log("  moved Plugins -> %s/%s", aside, pluginsDir)
	}
	if len(d.Extra) > 0 {
		aside := s.Root + "/Removed-" + time.Now().Format("20060102-150405")
		if _, err := r.run("mkdir "+q(aside), nil); err != nil {
			return fmt.Errorf("could not create %s: %w", aside, err)
		}
		for _, e := range d.Extra {
			dest := aside + "/" + e.Path
			cmd := "mkdir -p " + q(path.Dir(dest)) + " && test ! -e " + q(dest) + " && mv " + q(base+"/"+e.Path) + " " + q(dest)
			if _, err := r.run(cmd, nil); err != nil {
				return fmt.Errorf("moving %s aside: %w", e.Path, err)
			}
			log("  moved %s -> %s", e.Path, dest)
		}
		log("Removed items kept in %s", aside)
	}
	// Directories first (parents before children), then files, then permissions.
	missing := append([]entry{}, d.Missing...)
	sort.Slice(missing, func(a, b int) bool { return missing[a].Path < missing[b].Path })
	for _, e := range missing {
		if e.Kind == "dir" {
			if _, err := r.run("mkdir -p "+q(base+"/"+e.Path)+" && chmod "+e.Mode+" "+q(base+"/"+e.Path), nil); err != nil {
				return fmt.Errorf("recreating folder %s: %w", e.Path, err)
			}
			log("  recreated folder %s", e.Path)
		}
	}
	for _, e := range append(missing, d.Changed...) {
		switch e.Kind {
		case "file":
			data, err := os.ReadFile(filepath.Join(dir, "files", filepath.FromSlash(e.Path)))
			if err != nil {
				return err
			}
			live := base + "/" + e.Path
			tmp := live + ".nbtrestore"
			if _, err := r.run("mkdir -p "+q(path.Dir(live))+" && rm -f "+q(tmp)+" && cat > "+q(tmp), data); err != nil {
				return fmt.Errorf("writing %s: %w", e.Path, err)
			}
			back, err := r.run("cat "+q(tmp), nil)
			if err != nil || !bytes.Equal(back, data) {
				_, _ = r.run("rm -f "+q(tmp), nil)
				return fmt.Errorf("read-back check failed for %s; original left in place", e.Path)
			}
			if _, err := r.run("chmod "+e.Mode+" "+q(tmp)+" && mv -f "+q(tmp)+" "+q(live), nil); err != nil {
				return fmt.Errorf("putting %s in place: %w", e.Path, err)
			}
			log("  restored %s", e.Path)
		case "link":
			log("  NOTE: symlink %s -> %s was in the backup but is missing; recreate manually if needed", e.Path, e.Target)
		}
	}
	for _, e := range d.ModeOnly {
		if _, err := r.run("chmod "+e.Mode+" "+q(base+"/"+e.Path), nil); err != nil {
			return fmt.Errorf("chmod %s: %w", e.Path, err)
		}
		log("  chmod %s %s", e.Mode, e.Path)
	}
	after, err := compare(r, s)
	if err != nil {
		return err
	}
	if !after.empty() {
		return fmt.Errorf("after restore the folder still differs:\n%s", describe(after))
	}
	log("Verified: Plugins folder now matches the backup exactly.")
	return nil
}
