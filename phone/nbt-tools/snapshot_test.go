package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Simulates the head unit with a local folder and the local shell.
func fakeUnit(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	must := func(err error) {
		if err != nil {
			t.Fatal(err)
		}
	}
	must(os.MkdirAll(filepath.Join(root, "Plugins/StockApp/res"), 0o755))
	must(os.WriteFile(filepath.Join(root, "Plugins/StockApp/StockApp.manifest"), []byte("stock manifest\n"), 0o644))
	must(os.WriteFile(filepath.Join(root, "Plugins/StockApp/libStock.so"), []byte{0x7f, 'E', 'L', 'F', 0, 1, 2, 255}, 0o755))
	must(os.WriteFile(filepath.Join(root, "Plugins/StockApp/res/a b.png"), []byte("png with space"), 0o644))
	return root
}

func TestBackupModifyRestore(t *testing.T) {
	root := fakeUnit(t)
	out := filepath.Join(t.TempDir(), "bk")
	log := func(string, ...any) {}
	s, err := takeBackup(localRunner{}, root, "169.254.1.1", "fp", out, log)
	if err != nil {
		t.Fatal(err)
	}
	if s.HasGauges {
		t.Fatal("clean unit flagged as having gauge apps")
	}
	if n := len(s.Entries); n != 5 {
		t.Fatalf("want 5 entries, got %d: %+v", n, s.Entries)
	}

	// Simulate an install plus some damage.
	p := filepath.Join(root, "Plugins")
	os.MkdirAll(filepath.Join(p, "BoostDeck"), 0o755)
	os.WriteFile(filepath.Join(p, "BoostDeck/libBoostDeck.so"), []byte("app"), 0o644)
	os.WriteFile(filepath.Join(p, "StockApp/StockApp.manifest"), []byte("tampered"), 0o644)
	os.Remove(filepath.Join(p, "StockApp/res/a b.png"))
	os.Chmod(filepath.Join(p, "StockApp/libStock.so"), 0o644)
	os.WriteFile(filepath.Join(p, "StockApp/extra.txt"), []byte("x"), 0o644)

	loaded, err := loadSnapshot(out)
	if err != nil {
		t.Fatal(err)
	}
	d, err := compare(localRunner{}, loaded)
	if err != nil {
		t.Fatal(err)
	}
	if len(d.Extra) != 2 || len(d.Changed) != 1 || len(d.Missing) != 1 || len(d.ModeOnly) != 1 {
		t.Fatalf("unexpected diff:\n%s", describe(d))
	}
	if err := restore(localRunner{}, loaded, out, log); err != nil {
		t.Fatal(err)
	}
	after, _ := compare(localRunner{}, loaded)
	if !after.empty() {
		t.Fatalf("not restored:\n%s", describe(after))
	}
	// Extra items must be moved aside, never deleted.
	matches, _ := filepath.Glob(filepath.Join(root, "Removed-*/BoostDeck/libBoostDeck.so"))
	if len(matches) != 1 {
		t.Fatal("BoostDeck was not preserved in Removed-*")
	}
	if b, _ := os.ReadFile(filepath.Join(p, "StockApp/StockApp.manifest")); string(b) != "stock manifest\n" {
		t.Fatal("manifest content not restored")
	}
}

func TestRestoreRefusesWhileInstallerLocked(t *testing.T) {
	root := fakeUnit(t)
	out := filepath.Join(t.TempDir(), "bk")
	s, err := takeBackup(localRunner{}, root, "", "", out, func(string, ...any) {})
	if err != nil {
		t.Fatal(err)
	}
	os.Mkdir(filepath.Join(root, "GaugeDeck-install-lock"), 0o755)
	if err := restore(localRunner{}, s, out, func(string, ...any) {}); err == nil || !strings.Contains(err.Error(), "lock") {
		t.Fatalf("expected lock refusal, got %v", err)
	}
}

func TestDamagedLocalBackupIsRejected(t *testing.T) {
	root := fakeUnit(t)
	out := filepath.Join(t.TempDir(), "bk")
	if _, err := takeBackup(localRunner{}, root, "", "", out, func(string, ...any) {}); err != nil {
		t.Fatal(err)
	}
	os.WriteFile(filepath.Join(out, "files/StockApp/StockApp.manifest"), []byte("corrupt"), 0o600)
	if _, err := loadSnapshot(out); err == nil {
		t.Fatal("damaged backup accepted")
	}
}

func TestGaugeAppsMarkBackupNonFactory(t *testing.T) {
	root := fakeUnit(t)
	os.MkdirAll(filepath.Join(root, "Plugins/ShiftDeck"), 0o755)
	s, err := takeBackup(localRunner{}, root, "", "", filepath.Join(t.TempDir(), "bk"), func(string, ...any) {})
	if err != nil || !s.HasGauges {
		t.Fatalf("expected HasGauges, err=%v", err)
	}
}

func TestStockUnitWithoutPluginsFolder(t *testing.T) {
	root := t.TempDir()
	os.MkdirAll(filepath.Join(root, "RHMI-Cache"), 0o755)
	out := filepath.Join(t.TempDir(), "bk")
	log := func(string, ...any) {}
	s, err := takeBackup(localRunner{}, root, "", "", out, log)
	if err != nil {
		t.Fatal(err)
	}
	if !s.NoPlugins || s.HasGauges || len(s.Entries) != 0 {
		t.Fatalf("stock backup wrong: %+v", s)
	}
	loaded, err := loadSnapshot(out)
	if err != nil {
		t.Fatal(err)
	}
	if d, _ := compare(localRunner{}, loaded); !d.empty() {
		t.Fatalf("stock unit should match: %s", describe(d))
	}
	// Install creates Plugins with apps.
	os.MkdirAll(filepath.Join(root, "Plugins/BoostDeck"), 0o755)
	os.WriteFile(filepath.Join(root, "Plugins/BoostDeck/libBoostDeck.so"), []byte("app"), 0o755)
	d, _ := compare(localRunner{}, loaded)
	if !d.WholeFolder {
		t.Fatalf("expected whole-folder diff: %s", describe(d))
	}
	if err := restore(localRunner{}, loaded, out, log); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(root, "Plugins")); !os.IsNotExist(err) {
		t.Fatal("Plugins still present after restore")
	}
	m, _ := filepath.Glob(filepath.Join(root, "Removed-*/Plugins/BoostDeck/libBoostDeck.so"))
	if len(m) != 1 {
		t.Fatal("installed apps not preserved in Removed-*")
	}
}

func TestInstallAppStagesAndRefusesOverwrite(t *testing.T) {
	root := fakeUnit(t)
	app := filepath.Join(t.TempDir(), "FaultCodes")
	os.MkdirAll(app, 0o755)
	os.WriteFile(filepath.Join(app, "FaultCodes.manifest"), []byte("m"), 0o644)
	os.WriteFile(filepath.Join(app, "libFaultCodes.so"), []byte{0x7f, 'E', 'L', 'F'}, 0o644)
	os.WriteFile(filepath.Join(app, "fault-codes-qnx"), []byte("worker"), 0o644)
	log := func(string, ...any) {}
	if err := installApp(localRunner{}, root, app, "10.0.0.1", log); err == nil {
		t.Fatal("non-ENET gateway accepted")
	}
	if err := installApp(localRunner{}, root, app, "169.254.10.1", log); err != nil {
		t.Fatal(err)
	}
	dest := filepath.Join(root, "Plugins/FaultCodes")
	if b, _ := os.ReadFile(filepath.Join(dest, "gateway-ip.txt")); string(b) != "169.254.10.1\n" {
		t.Fatalf("gateway-ip.txt = %q", b)
	}
	if st, _ := os.Stat(filepath.Join(dest, "fault-codes-qnx")); st == nil || st.Mode().Perm() != 0o755 {
		t.Fatal("worker not executable")
	}
	if m, _ := filepath.Glob(filepath.Join(root, ".nbt-stage-*")); len(m) != 0 {
		t.Fatal("staging folder left behind")
	}
	if err := installApp(localRunner{}, root, app, "169.254.10.1", log); err == nil || !strings.Contains(err.Error(), "already installed") {
		t.Fatalf("expected refusal to overwrite, got %v", err)
	}
	os.Mkdir(filepath.Join(root, "GaugeDeck-install-lock"), 0o755)
	os.RemoveAll(dest)
	if err := installApp(localRunner{}, root, app, "169.254.10.1", log); err == nil || !strings.Contains(err.Error(), "lock") {
		t.Fatalf("expected lock refusal, got %v", err)
	}
}
