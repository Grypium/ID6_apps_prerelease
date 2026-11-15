# Doing everything from an Android phone (no laptop)

This folder lets you unlock SSH, back up, install the apps and read fault codes on a
BMW NBT Evo head unit using only an Android phone running [Termux](https://termux.dev),
a USB-C Ethernet adapter and an ENET (OBD to RJ45) cable.

It was worked out and tested end to end on a Samsung Galaxy (Android 15) against an
`NBTevo_Y24073J` head unit (1280 × 480), installing the official
BoostDeck / ShiftDeck / MHD Maps payload and the new FaultCodes app.

> Everything here is at your own risk. Work parked, keep the battery healthy, and
> take the factory backup (step 4) before installing anything.

## What's in here

| Path | What it does |
| --- | --- |
| `bin/eth-fwd` | Localhost relay whose outgoing sockets are bound to the Ethernet network (see "Why a relay" below). |
| `bin/ui` | Tap through Android Settings over ADB (`ui dump`, `ui tap "Text"`). |
| `bin/nbt-installer-android` | Runs the installer with its car sockets bound to Ethernet. |
| `bavarian-ssh/enable-ssh.sh` | Runs the official Bavarian SSH Client (Linux x86-64) headless under emulation and presses Enable SSH. |
| `installer/` | Patch + build script for the official Go installer (last upstream version with an installer) for Android, plus `extract_payload.py`, which pulls the official app payload out of the Windows release in git history. |
| `nbt-tools/` | `nbt-backup` (backup / verify / restore / install-app), `hu` (run one command on the head unit), `hu-get` (fast file copy). |
| `qnx-clang-kit/` | Builds the QNX 6.5 ARM apps with Termux `clang` + `lld`, without the QNX SDK. |

## One-time phone setup

```sh
pkg install git golang clang lld python python-pillow proot-distro android-tools termux-api
termux-setup-storage
```

**ADB to the phone itself.** Several steps read Android's network state (`dumpsys`) or tap
Settings. Enable Developer options → Wireless debugging, pair Termux's `adb` with the
phone once (`adb pair`), then `adb tcpip 5555` so `adb -s localhost:5555` works.

**Bavarian SSH Client under emulation** (only needed to unlock SSH):

```sh
proot-distro install -a x86_64 -n deb64 debian:12
proot-distro login deb64 -- apt-get install -y xvfb xdotool imagemagick tesseract-ocr
```

Download the official Linux build of the Bavarian SSH Client yourself and put the
`bavarian-ssh-client` binary next to `enable-ssh.sh` (it is not redistributed here).

**Build the tools:**

```sh
cd phone/nbt-tools && go build -o ~/nbt-backup . && go build -o ~/bin/hu ./cmd/hu && go build -o ~/bin/hu-get ./cmd/hu-get
git fetch origin upstream-old-main      # preserved upstream installer + release history
phone/installer/build-installer.sh      # -> ~/nbt-installer-enet (official payload embedded)
cp phone/bin/* ~/bin/
```

## At the car

### 1. Ethernet with a static link-local IP

Android does not self-assign a 169.254.x.x address on Ethernet, so set one:

Settings → Connections → More connection settings → Ethernet. **Turn Ethernet off
first**, otherwise Save stays greyed out. Configure eth0 → Static IP:

| Field | Value |
| --- | --- |
| IP address | `169.254.199.99` (any unused 169.254.x.x) |
| Netmask | `255.255.0.0` |
| DNS | `8.8.8.8` (required by the form, unused) |
| Default gateway | `169.254.199.1` (required by the form, unused) |

Save, then turn Ethernet back on. Check with
`adb -s localhost:5555 shell ip -4 addr show eth0`.

### 2. Why a relay: Android routes apps over Wi-Fi / mobile data

Even with the cable up, an app's traffic to 169.254.x.x goes out over Wi-Fi and times
out. A socket only reaches the car if it is bound to the Ethernet network
(`android_setsocknetwork`). `eth-fwd` and the patched installer do that.

Find the car (the broadcast discovery does reach it):

```sh
cd nbt-evo-trio-collector && go run . --find-car   # prints the gateway IP
adb -s localhost:5555 shell 'ping -b -c 2 169.254.255.255; ip neigh show dev eth0'
```

The head unit is the neighbour whose MAC starts `00:1c:d7`; the gateway answers on
TCP 6801.
Start the relay with your values and leave it running:

```sh
eth-fwd 6801:GATEWAY_IP:6801 2222:HEAD_UNIT_IP:22 &
```

### 3. Unlock SSH

```sh
phone/bavarian-ssh/enable-ssh.sh 127.0.0.1    # through the relay; takes ~1-2 minutes emulated
```

Look for `RESULT: SSH is enabled.` A screenshot is saved as `last-screen.png`.

### 4. Factory backup (before installing anything)

```sh
NBT_PW='<root password>' nbt-backup backup --head HEAD_UNIT_IP --via 127.0.0.1:2222
nbt-backup list
```

- The first connection shows the head unit's SSH key fingerprint; confirm it. It is
  pinned in `~/nbt-backups/known_hosts`.
- A unit that never had apps has **no `/var/opt/hmi/data/ID5/Plugins` folder**. The
  backup records that as the factory state; restoring it later moves the whole
  Plugins folder aside.

### 5. Install the official apps

The official installer refuses if `ID5/Plugins` is missing. iDrive scans
`/var/opt/hmi/data/rw/Plugins/` (`rw` → `ID5`) and just logs "not found" when absent,
so create it:

```sh
NBT_PW='<root password>' hu 'mkdir /var/opt/hmi/data/ID5/Plugins && chmod 755 /var/opt/hmi/data/ID5/Plugins'
nbt-installer-android            # opens the page in the browser
```

In the page: enter the head unit IP (broadcast discovery only finds the gateway),
choose the resolution if auto-detect fails (it does over SSH: `LayerManagerControl`
needs `LD_LIBRARY_PATH=/opt/sys/lib`; check with
`hu 'LD_LIBRARY_PATH=/opt/sys/lib /opt/sys/bin/LayerManagerControl get screen 0'`),
verify the fingerprint, check compatibility, install. Then, parked, hold the
volume/power knob ~30 s until iDrive restarts and open My Vehicle.

### 6. FaultCodes app (read-only fault memory of every module)

```sh
qnx-clang-kit/stubs/build-stubs.sh
qnx-clang-kit/build.sh source/native ~/qnx-out
mkdir -p ~/FaultCodes && cp source/native/plugin/FaultCodes.{manifest,xml} ~/qnx-out/{libFaultCodes.so,fault-codes-qnx} ~/FaultCodes/
python3 source/native/tools/make_fault_art.py ~/FaultCodes/FaultCodes_ALL_Images.zip
NBT_PW='<root password>' nbt-backup install-app --head HEAD_UNIT_IP --via 127.0.0.1:2222 --app ~/FaultCodes --gateway GATEWAY_IP
```

`install-app` stages the folder outside Plugins, reads every file back, writes
`gateway-ip.txt`, and only then moves it into place. It never overwrites an existing
app. Restart iDrive; "Fault Codes" appears under My Vehicle.

### Reset to factory

```sh
NBT_PW='<root password>' nbt-backup verify  --head HEAD_UNIT_IP --via 127.0.0.1:2222   # read-only diff
NBT_PW='<root password>' nbt-backup restore --head HEAD_UNIT_IP --via 127.0.0.1:2222 --yes
```

Nothing is deleted on the car: added items are moved to `ID5/Removed-<timestamp>/`.

## FaultCodes: how it works

It shows the engine view a tuning tool shows: the DME's fault memory and its
shadow memory, nothing from other modules. `fault-codes-qnx`
(`source/native/src/fault_scan.c`, built from `collector.c` with `-DFAULT_SCAN_ONLY`)
sends two read-only physical requests to the DME (0x12) through the gateway over HSFZ:

- `19 02 0C`: ReadDTCInformation, pending + confirmed codes (fault memory)
- `22 20 00`: ReadDataByIdentifier 0x2000, BMW's shadow memory (de-duplicated)

It uses its own tester address (0xF5) so its replies are not mixed with the gauge
worker's (0xF4), and retries if the DME is busy. Nothing is written or cleared.

The plugin shows four codes per page (Next page on screen or in Options):
code + module, and status + description from `dme_descriptions.h`. Shadow codes
are labelled "Shadow".

## Tips and gotchas

- QNX's shell on the head unit has no `printf`, `head` or `which`; use `print -r --`.
- The head unit's old sshd truncates long single transfers around 1 MB. `hu-get`
  copies in verified 256 KB chunks over one connection (60 MB in ~25 s).
- `pkill -f PATTERN` inside a `bash -c` whose own command line contains PATTERN kills
  that shell too; kill by PID.
- The QNX shell over SSH has no `LD_LIBRARY_PATH`; set it for system tools.
