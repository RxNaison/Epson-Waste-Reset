# EWR (Epson Waste Reset)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20Android-blue)
![C++](https://img.shields.io/badge/language-C++17-orange)
![License](https://img.shields.io/badge/license-Apache_License_2.0-green)

A free, cross-platform, and completely open-source C++ utility to reset the "Waste Ink Pad" counter on Epson printers. 

EWR bypasses the need to pay for sketchy third-party reset keys (like WIC Reset) or run malicious, virus-flagged `AdjProg.exe` binaries. By dynamically generating IEEE 1284.4 hardware packets and utilizing a continuously updated database, EWR communicates directly with the printer's motherboard over USB to safely zero out the EEPROM waste counters.

<p align="center">
  <a href="https://github.com/RxNaison/Epson-Waste-Reset/releases/latest">
    <img src="https://img.shields.io/badge/DOWNLOAD%20LATEST%20RELEASE-2ea44f?style=for-the-badge&logo=github&logoColor=white" alt="Download the latest EWR release">
  </a>
</p>

## Features

* **Reads before it writes, and verifies after.** Every reset runs the same lifecycle: read the current counters, refuse to continue if the printer reports a state that makes the reset pointless or unsafe, ask you once, write, then read the counters back and compare. You see the numbers move instead of trusting an acknowledgement.
* **Every write can be undone.** Before the first byte is written, EWR saves the bytes it is about to overwrite to `backups/`. `--restore` puts them back - from that backup, or from an `--dump` file - through the same gates and read-back as a reset.
* **Printer maintenance, too.** Nozzle check, head cleaning, power cleaning and the printer's own counters (pages, cleanings, firmware, serial number), over USB or the network - behind flags and the API, so a double-click still does exactly one thing: the reset.
* **Smart Protocol Engine:** Constructs exact EEPROM write packets (`|B`) on the fly for your specific model. It manages the IEEE 1284.4 (D4) credit system properly, so nothing overflows a buffer or locks the printer up.
* **~1450 printers**, from Stylus Photo R-series to the current ET / L / XP / WF lines. Cartridge ink levels can be reset too on models that expose a per-colour map.
* **OTA Database Sync:** The printer database refreshes itself at startup, before anything is read from or written to the printer, so a database fix applies to the run that fetched it and the database in use never changes underneath a reset. The full database also ships in the download - EWR works offline out of the box.
* **Cross-Platform Core:**
  * **Windows (x64 and ARM64):** Native Win32 `SetupAPI` with asynchronous `OVERLAPPED` I/O to safely drain the Windows Print Spooler buffers, plus a statically linked `libusb` for the vendor-specific interfaces `usbprint.sys` cannot reach - on ET-2xxx units the maintenance engine lives on one of those. Both transports run in one pass, `usbprint.sys` first. Zero custom drivers required.
  * **Linux (x86_64, aarch64, armhf - a Raspberry Pi too) & macOS (one universal binary for Intel and Apple Silicon):** Uses `libusb` to automatically detach the kernel driver (CUPS) for exclusive, raw hardware access. `.deb` and `.rpm` packages install a udev rule, so no `sudo` is needed.
  * **Android:** an app that reaches the printer over an OTG cable or Wi-Fi, built on the same library.
* **Zero Hardcoded PIDs:** Scans your OS USB tree to find connected Epson printers and identifies them from the IEEE 1284 device ID they report.
* **A trace log for every run.** `ewr_trace.log` records every byte in both directions next to the binary. It is what makes a bug report solvable.
* **Replay Fallback:** If your printer is too new for the database, EWR can still parse and execute a raw Wireshark dump you supply (stripping USBPcap headers automatically).

## Usage

1. Download the latest version from the [Releases page](https://github.com/RxNaison/Epson-Waste-Reset/releases/latest) (or click the big green button above) and unzip it anywhere. Keep `ewr` and `database.json` together.
2. Turn your Epson printer on and connect it **via USB**, or have it on the same network as your computer: when nothing answers on USB, EWR searches the network and asks which printer to use. `--ip <address>` (below) goes straight to one.
3. Run it:
   * **Windows:** double-click `ewr.exe`
   * **Linux / macOS:** `sudo ./ewr` *(raw USB access requires root)*
4. EWR detects the printer and offers the matching database entry. Press Enter to accept it, or type part of a model name to search for another.
5. It reads the printer's status and current counter values and prints them, then asks for confirmation once. Answer `y`.
6. It saves a backup of the bytes it is about to overwrite to `backups/`, writes, then reads the counters back and tells you whether every one now holds its reset value.
7. **Turn the printer off and back on with its physical power button** to commit the change.

No internet connection is needed - the database ships in the download. When EWR can reach GitHub it picks up database updates at startup.

### Verifying your download

Download EWR only from the [Releases page](https://github.com/RxNaison/Epson-Waste-Reset/releases/latest). Copies passed around elsewhere - in video descriptions, on forums - may not be what they claim to be. From v1.4.1 on, every release archive, including the `EWR-SDK-*` ones, carries a signed build-provenance attestation tying it to this repository and to the exact build that produced it. You can check it with the [GitHub CLI](https://cli.github.com/), signed in (`gh auth login`):

```bash
gh attestation verify EWR-v1.5.1-win.zip -R RxNaison/Epson-Waste-Reset
```

The same works for every archive and package: `EWR-<tag>-win.zip`, `-win-arm64.zip`, `-linux.tar.gz`, `-linux-aarch64.tar.gz`, `-linux-armhf.tar.gz`, `-macos-universal.tar.gz`, `-android.apk`, the `.deb` and `.rpm` packages, and the `EWR-SDK-*` archives.

`✓ Verification succeeded!` means the file is byte for byte what this repository's release workflow built, and its `Build workflow` line ends in the tag it was built from (`@refs/tags/v1.5.1`) - check that it is the version you meant to download. A modified file fails with `HTTP 404: Not Found`: no attestation exists for its contents. If an archive from v1.4.1 or later fails, don't run it. Releases before v1.4.1 have no attestation and fail the same way, so the check cannot vouch for them. This is not code signing: Windows may still warn that the app is unrecognized.

### Command-line options

Running with no options is the supported path. These exist for diagnosis and for people packaging EWR into something larger.

| Option | What it does |
| --- | --- |
| `--status`, `-s` | Read-only: printer status, ink levels and waste counter values. Sends no writes. |
| `--list`, `-l` | List every Epson USB interface, and every Epson printer that answers on the network, with its IEEE 1284 device ID and database match, then exit. Read-only. |
| `--model <name>` | Skip the menu. Takes the exact name, an alias, or the name the printer reports (`--model "EPSON ET-2803 Series"`). |
| `--ip <address>` | Reach a printer on the network instead of USB, over SNMP (UDP 161): `ewr --ip 192.168.1.100`. Works with `--status`, `--dry-run`, the waste ink pad reset, `--restore`, `--info` and the maintenance commands, with the same gates, confirmation and read-back as USB. Replay dumps, the cartridge ink reset, `--dump` and the read-key search still need USB. Many recent models lock the EEPROM over the network but not over USB; EWR says so and stops before writing anything. |
| `--ip auto` | Search the network and pick the printer from a list, even when another printer is on USB. It asks at the keyboard, so it does not combine with `--yes` or `--json`: pass the address from `--list` instead. |
| `--interface <n>` | Pin the run to interface `<n>` from `--list` and disable the automatic fallback. |
| `--dry-run` | Detect, read, and show exactly what a reset *would* write - then stop. |
| `--dump` | Read the EEPROM into a timestamped file. Dump twice around a change and diff to map an unknown printer. `--full` reads every byte up to the model's memory limit instead of the low page. |
| `--restore <file>` | Undo a write: put back what a backup from `backups/`, or an `--dump` file, holds of this model's counter bytes - only those, only the ones that differ. Same gates, confirmation and read-back as a reset; `--dry-run` shows the plan. |
| `--info` | Read-only: firmware version, serial number, the printer's own counters (pages printed, cleanings, ink changes, scans) and the codes of its last fatal errors, where the database knows them. |
| `--nozzle-check` | Print the nozzle check pattern. Load plain paper. `--nozzle-check alt` asks for the alternate pattern some models print. |
| `--head-clean [level]` | A head cleaning: `gentle`, `normal` (the default), `medium` or `strong`, as far as the model lists them. `--clean-group black` or `color` cleans only those nozzles. Uses ink, and the ink goes into the waste pads. |
| `--power-clean` | The ink charge: a much stronger cleaning that uses a lot of ink. Only when normal cleanings did not clear the nozzles. Asks you to type `charge`. |
| `--print-data` | Send a cleaning as print data (ESC/P2 remote mode) even where the model has its own maintenance command - for a printer that refuses that one. |
| `--find-addresses` | For a model with no reset addresses yet: read the EEPROM three times with a head cleaning between reads - EWR runs them, or you do from the panel - and report the bytes that rise every time. No EEPROM byte is written. |
| `--find-key` | For a printer the database has no entry for: try every read key in the database and report which one, if any, the printer answers. Takes no `--model`. Read-only. |
| `--all-keys` | With `--find-key`, and only if no database key fits and the printer answered them at all: sweep every 2-byte key, families closest to the known ones first, and stop at the first that answers. Read-only. A key near a known family usually turns up in a few minutes; the whole range takes hours, and Ctrl-C is safe throughout. A key found this way opens `--status` and `--dump`, never a reset - the reset addresses are still unknown. |
| `--yes`, `-y` | Answer the reset confirmation with yes, for callers driving EWR non-interactively. See below. |
| `--force-yes` | `--yes`, and overrule the gates that would stop it: a model mismatch, a printer error, a database conflict. Last resort. |
| `--cartridge`, `-c` | Reset the cartridge ink levels instead of the waste ink pads, on models that carry a per-color ink map. |
| `--no-update` | Fully offline: no update check, no download, `database.json` left as it is. Use it while editing `database.json`. |
| `--usb-soft-reset` | Diagnostic only, Windows. Resets the USB channel once at the start of the run, then waits (up to 90 s) for the printer to finish the re-initialization the reset starts. |
| `--json` | Machine-readable output: one JSON object per line on stdout, for callers driving EWR from another language. See [docs/json-output.md](docs/json-output.md). |
| `--help`, `-h` | The same list, from the binary. |

On a terminal, EWR colors its output: message tags, ink and waste pad gauges, the printer's state. Set `NO_COLOR=1` to turn that off. Piped output, log files and `--json` never carry color codes.

### Unattended runs: `--yes`

```bash
ewr --yes                      # reset the printer that answers, unattended
ewr --model L3150 --yes        # ... or name the model yourself
```

`--yes` answers one question: the final *"Reset the waste ink pad counters now?"*. Everything
else about the run is unchanged - the preflight read, the read-back verification, and every
gate that can stop it.

Without `--model` it takes the model the printer reports over IEEE 1284, the same entry the
menu offers on Enter, and stops if the printer names nothing the database recognises. So the
model it writes to is the one the printer identified itself as - never a guess and never a
menu default. What it deliberately does **not** do:

* **It does not overrule the detected model.** If `--model` names something other than the
  printer that answered, the run stops instead of writing. A wrong-model write is the hazard
  the confirmation exists for, and no flag should be able to cause one.
* **It does not push past a printer error or a database conflict.** Those gates report what
  the printer said, not what you intended, so `--yes` declines them and exits non-zero.
* **It never arms the cartridge ink reset by itself.** On a model that offers both, `--yes`
  takes the waste ink pads. Reaching the ink reset takes `--cartridge`, named explicitly.
* **It does not search the menu for you.** It takes the detected model or it stops; it never
  falls back to picking an entry from a list.

Exit status is `0` when the reset completed and verified, `1` when it was stopped or failed,
`2` for a bad command line. Writes the printer acknowledged but did not keep are a failure:
the read-back shows the old values, and the run says so instead of claiming success.

Two EWR runs must not drive one printer: they share the USB handle and take each other's
replies, and the run that loses one may be the one mid-write. A second run stops with a
message instead of starting. The closing "Press Enter to exit" is skipped when EWR is not
run from a terminal, so a caller that pipes its answers never waits on a keypress.

### Driving EWR from another language: `--json`

`--json` turns stdout into one JSON object per line: a `hello`, an `event` per step, and
one `result` carrying the outcome. Any language that can start a process can use it, and a
caller that reads only `result` and the exit code keeps working across EWR versions -
[docs/json-output.md](docs/json-output.md) is the contract and what it promises.

```python
import json, subprocess

run = subprocess.run(
    ["./ewr", "--model", "L3150", "--yes", "--json", "--no-update"],
    stdout=subprocess.PIPE, stdin=subprocess.DEVNULL, text=True,
)

result = None
for line in run.stdout.splitlines():
    msg = json.loads(line)
    if msg["type"] == "event" and msg["total"]:          # progress, e.g. 3 of 5 writes
        print(f"{msg['index']}/{msg['total']} {msg['code']}")
    elif msg["type"] == "result":
        result = msg

if result["ok"]:
    print("verified", result["data"]["writes"]["verified"], "of", result["data"]["writes"]["total"])
elif result["error_code"] == "blocked":
    print("a gate stopped it, nothing was written:", result["error"])
else:
    print("failed:", result["error_code"], result["error"])
```

`--json` never prompts, so answer the gates on the command line: `--yes` for the reset
confirmation, `--force-yes` to overrule a printer error as well. Without `--model` it uses
the model the printer reports. A run that would need an answer stops with
`error_code: "blocked"` and writes nothing.

To put the reset *inside* your program instead of spawning it - a GUI, a service, a
language binding - there is a C ABI in `include/ewr/ewr_c.h`, built as `ewrc.dll` /
`libewrc.so` alongside the CLI, with the same JSON coming back:
[docs/c-api.md](docs/c-api.md).

### Choosing the target: `--cartridge`

Some models carry a per-color cartridge ink map as well as waste pad counters, and a run
without flags offers the choice. `--cartridge` makes that choice on the command line instead:

```bash
ewr --model R220 --cartridge              # ask, then reset the ink levels
ewr --model R220 --cartridge --yes        # ... without asking
ewr --model R220 --cartridge --dry-run    # show the ink writes, send nothing
```

It selects the target and nothing else - it is not a confirmation. On its own the reset still
arms on the word `reset` typed at the keyboard; `--yes` is what answers that. `--dry-run`
follows it, so the printed plan is the plan of the run you asked for.

EWR cannot refill ink. This reset rewrites the printer's accounting, so a genuinely empty
cartridge will report full and can run dry mid-print, which damages the print head. It also
only holds on printers that keep ink levels in their own EEPROM: where the cartridge carries a
smart chip, the firmware treats the chip as the truth and writes the EEPROM back from it.

### Undoing a reset: backups and `--restore`

Every reset - and every restore - first saves the bytes it is about to
overwrite, as the preflight read them, to `backups/` next to `ewr` (or to your
user folder when EWR cannot write there: `%LOCALAPPDATA%\EWR`,
`~/.local/share/ewr`, `~/Library/Application Support/EWR`). The path is printed
before the first write. If the backup cannot be saved, or the printer did not
answer for every byte, EWR stops and asks rather than writing something it
could not undo.

```bash
ewr --restore backups/L3150_X5NA012345_20261024T183651Z.json --dry-run   # what it would write
ewr --restore backups/L3150_X5NA012345_20261024T183651Z.json             # put the bytes back
ewr --restore ewr_dump_L3150_1736900000.txt                              # from a dump, any version
```

A restore writes only the bytes a reset of that model writes - its waste and
ink counters - and only those that differ from what the printer holds now, so
it is never more of a write than a reset. It names the bytes it leaves alone
and why. A file from another model is refused; one from another printer of the
same model (a different serial number) asks first. A dump made before 1.6
stops at `0xFF`, so on models with counters above that the restore says which
bytes it cannot put back.

Restoring an old backup puts the counters back where they were: a printer whose
pads were full before the reset will say so again.

### Maintenance: nozzle check and head cleaning

```bash
ewr --nozzle-check                 # print the pattern (load plain paper)
ewr --head-clean                   # a normal cleaning
ewr --head-clean strong --clean-group black
ewr --power-clean                  # the ink charge - a lot of ink
ewr --info                         # firmware, serial, page and cleaning counters
```

These write no EEPROM byte, but they act on the printer: a cleaning uses ink
and fills the waste pads. So they run the way a reset does - the printer's
state is read first, a printer that reports an error is not sent anything
(a cleaning with an empty cartridge draws air into the head), you are asked
once, the command is sent exactly once, and EWR watches the printer's status
until it is idle again, then shows what the cleaning did to the pad counters.
A printer whose pads already count as full refuses maintenance: reset first.

Where the model's database entry lists its cleaning levels, EWR uses the
printer's own maintenance command; otherwise it sends the cleaning as print
data (ESC/P2 remote mode), which has only the printer's normal cleaning. Over
the network that goes to the printer's raw print port (TCP 9100).

### Other platforms

* **Linux packages:** `ewr_<version>_<arch>.deb` and `ewr-<version>.<arch>.rpm`
  for `amd64`/`x86_64`, `arm64`/`aarch64` and `armhf`/`armv7hl`. They install
  `/usr/bin/ewr`, the database and a udev rule that lets the logged-in user
  reach an Epson printer without `sudo`. `ewr-dev`/`ewr-devel` carries the C API.
* **Raspberry Pi:** the `linux-armhf` (32-bit) or `linux-aarch64` (64-bit)
  archive, or the matching package.
* **macOS:** one universal binary for Intel and Apple Silicon Macs.
* **Android 8.0 or newer (`EWR-<tag>-android.apk`):** connect the printer with
  a USB OTG cable, or reach it over Wi-Fi. Plugging the printer in offers to open EWR.
  The app does what the command line does - status, reset, backups and
  restore, maintenance, info - with the same gates and confirmations.

### Overriding the gates: `--force-yes`

`--force-yes` is `--yes` plus the two refusals above: it writes to a model the printer did not
report as itself, and it writes while the printer objects with an error or the database
disagrees with itself about this model's write path.

That is the combination this tool spends most of its code preventing, and every reason those
gates exist still applies when you pass it - a wrong-model write can misconfigure a printer
you cannot replace. It is here for the case where you know the detection is wrong and the
printer is right, not as an automation default. Prefer `--model` over forcing a mismatch.

What it does **not** touch: the preflight read, the address-width encoding guards, and the
read-back verification. Those are not prompts, they are the write path, and no flag skips them.

## When something goes wrong

Every run writes **`ewr_trace.log`** next to the executable, containing every packet sent and received. If EWR fails, or claims success and the error comes back after a power cycle, that file is what makes the problem solvable - please attach it to an issue rather than pasting the console output alone.

Useful first steps:

* `ewr --list` shows every USB interface EWR can see and which one it would pick. On a printer that also scans, several will be listed.
* `ewr --status` reads the counters without writing anything, so it is always safe to run.
* If the printer says the **maintenance box** needs replacing rather than the ink pads, that box has its own chip - replace it or use a physical chip resetter. EWR resets the printer's internal counter, which is a different thing.

If the printer does not answer, reports as busy, or the reset fails, try these before opening an issue - they settle most reports:

1. **Close all other Epson software** - Epson Scan, Epson Event Manager, and Epson Status Monitor in the Windows system tray. A program talking to the printer at the same time can keep it busy or take EWR's replies.
2. **Cancel every print job waiting for the printer.** A job sent before the error appeared stays in the queue, and while it waits the printer can refuse EWR's data. Open the printer's print queue (on Windows: Settings > Bluetooth & devices > Printers & scanners > your printer > Open print queue) and cancel everything in it. That alone is not enough: the printer keeps the part of the job it already received until the next step.
3. **Power-cycle the printer**, after the queue is empty - with a job still queued, the computer sends it again as soon as the printer is back. Turn it off, then on, and wait until it has finished starting up before running EWR.
4. **Use a USB port directly on the computer** (on a desktop, one at the back) - avoid hubs, front-panel ports and USB-C adapters.
5. **Install Epson's own driver** from [epson.com/support](https://epson.com/support). On Windows 11, check first which driver Windows installed: `Get-Printer` in PowerShell lists each printer with its driver. **Microsoft IPP Class Driver** means Windows Ready Print is driving the printer, and with it an XP-332 never answered EWR at all (`ewr_trace.log` said `Printer did not answer the D4 Init transaction`). Remove the printer in Settings > Bluetooth & devices > Printers & scanners, install Epson's driver, and run EWR again.

## Building from Source

### Prerequisites
* **Windows (x64 or ARM64):** Visual Studio with MSVC C++ build tools. `libusb` is fetched and built by CMake, so the first configure needs `git` and a network connection; it is then linked statically and `ewr.exe` ships alone.
* **Linux (Arch/Debian, any architecture):** `cmake`, `gcc`, `git`, `pkgconf` and `libcurl4-openssl-dev`. `libusb-1.0-dev` too, unless you pass `-DEWR_BUNDLED_LIBUSB=ON`, which builds the pinned libusb and links it statically as the releases do.
* **macOS:** `brew install cmake pkgconf` - libusb is built from the pinned source and libcurl comes with the system. Add `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` for a universal binary.
* **Android:** Android Studio, or the SDK with NDK `27.0.12077973` and JDK 21: `cd android && ./gradlew assembleDebug`. The app builds the library from this repository; nothing is copied.

```bash
# 1. Generate the build files
cmake -B build

# 2. Compile the project (Release mode)
cmake --build build --config Release
```

The compiled executable (`ewr.exe` or `ewr`) is placed in the repository root - the build pins the output directory there so the binary sits next to `database.json` and the `models/` folder. Both are found relative to the executable, not your shell's working directory.

Running the test suite:

```bash
cmake -B build -DEWR_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

`-DEWR_RESTORE_ALL=ON` adds `--restore-all`, which writes every byte of a dump of the same printer back, not only its counters. It is off in releases until it has been verified on more hardware.

On macOS the downloaded binary is quarantined by Gatekeeper. Clear it with `xattr -d com.apple.quarantine ewr`, or right-click the binary and choose **Open**.

## 🤝 Contributing a New Printer Model

Adding or fixing a printer means **editing `database.json` and opening a pull request** - no C++ required. `database.json` is the curated source of truth: the weekly rebuild merges upstream sources *around* your values and can never overwrite a hand edit. See **[CONTRIBUTING.md](CONTRIBUTING.md)** for the field reference and what makes a good pull request.

If your model is missing entirely, two things have to be found: a read key the printer answers to, and then its addresses. Start with the safe methods - EWR ships the tooling for both, and neither writes the EEPROM.

### Is there a key for it? (read-only)

EWR cannot read a printer without its read key. Printers of one generation often share keys, so a missing model may answer to a key the database already has for a sibling:

```
ewr --find-key
```

It reads one byte with each of the ~130 distinct read keys in the database - a few minutes - and reports how the printer answered. A key that draws no answer is asked again, up to three times, before it is counted as unanswered:

* **A value came back**: that key works. The result names the database entries that use it; read the printer with one of them (`--dump`, `--find-addresses` below). Do not run a reset with that entry - the key fits, but its addresses are that model's, not yours.
* **Every key was refused (`:41:NA;`)**: the printer knows the command and its key is not in the database.
* **Every key drew an empty answer (`||:;`)**: the printer most likely does not take this read command at all.

Either way, attach `ewr_trace.log` to an issue: it records how every key was answered.

### Finding the counters yourself (no EEPROM writes)

Around 113 database entries carry a working read key but no reset addresses - EWR can read those printers, it just does not know which bytes hold the waste counter. They appear in the menu as `(no reset addresses yet - read-only)`.

`--find-addresses` goes looking. It reads the EEPROM three times with a head cleaning between reads, because a cleaning is the one action guaranteed to move the waste counter. EWR offers to run the cleanings itself; answer `n` to start each one from the printer's panel instead. On a printer that did not match the entry you named - one `--find-key` found a sibling's key for - EWR asks for the printer's own normal cleaning, sent as print data, never that entry's cleaning command: one model's cleaning byte can be another model's ink charge.

```
ewr --model ET-M1180 --find-addresses --no-update
```

Bytes that move the **same way after both** cleanings are reported, rising or falling, with their three readings side by side; a byte that moves only once is a timer or a page count and is dropped. A waste counter only ever rises, so the rising rows are the candidates and a falling one is something else - an ink level, a countdown. Two intervals is the minimum that tells those apart, which is why it takes three reads. Little-endian pairs are handled, so a low byte rolling over into its neighbour is reported as one 16-bit counter rather than discarded for falling. No EEPROM byte is written at any point, and the result is saved as `<Model>-found-addresses.json`.

Budget a few minutes: a read of the low page takes about a minute and a half on older models, and seconds where the printer answers range reads; it runs three times with two cleanings in between. A cleaning also uses real ink and pushes the waste counter up.

**This records readings, not a reset plan.** The saved file's `pad_groups` is empty on purpose: which of these is a waste counter, and what it resets to, is a judgement no dump can make - some pad groups reset certain bytes to `0x5E` (94) rather than 0. Open an issue or a pull request with the file rather than writing anything back blind.

If you would rather do it by hand, `--dump` writes a single timestamped snapshot you can `diff` yourself:

```
ewr --model L3150 --dump --no-update
diff ewr_dump_L3150_1736900000.txt ewr_dump_L3150_1736903600.txt
```

A `--` is a byte the printer never answered, so its value is unknown. A dump with any `--` is reported as incomplete and exits `1` instead of `0`; run it again rather than keeping it as a backup. If every value comes back as `--`, the read key is wrong for your printer - try a different sibling from the same model line.

Mind the mirror trap: a byte that returns to its old value by itself after a power cycle is being rewritten by the firmware from the cartridge chip. That level lives on the chip and cannot be reset from the PC.

Put what you find into `database.json`, check it with `--dry-run`, confirm with a real run, and open a pull request with the console output.

<details>
<summary>Last resort: capturing Epson's adjustment program</summary>

Only worth trying if dump-and-diff gets you nowhere - for example when no sibling read key answers at all.

This means running an unsigned "adjustment program" downloaded from a file-sharing site. Those binaries are frequently malware and several are flagged by antivirus for good reason. **Use a throwaway virtual machine with no access to anything you care about, or skip this entirely.** Not having to run that software is the reason EWR exists.

**Step 1: capture the conversation**
1. Install [Wireshark](https://www.wireshark.org/) in the VM (ensure **USBPcap** is installed on Windows).
2. Connect the printer to the host, turn it on, then pass it through to the VM.
3. Start capturing on the USB interface.
4. Run the adjustment program's "Reset Waste Counters" command inside the VM.
5. Stop the capture as soon as it tells you to power-cycle the printer.

**Step 2: export the payloads**
1. Apply this display filter:
   `usb.endpoint_address.direction == 0 && usb.transfer_type != 0x02`
   *(this isolates the `URB_BULK out` packets sent to the printer)*
2. **File** → **Export Packet Dissections** → **As C Arrays...**
3. Save it under the model name, e.g. `L3150.c`.

**Step 3: turn it into a database entry**
1. Create a `models/` folder next to the binary and drop the `.c` file in. EWR strips the Wireshark metadata and offers it as a `(Replay)` option, which is enough to confirm the capture is usable.
2. Read the addresses and keys out of the capture, add them to `database.json` as a normal entry, and open a pull request.

A database entry is strictly better than shipping the replay dump: it shows the counter values before and after and verifies the read-back, which a blind replay cannot. Replay dumps stopped being bundled with releases for that reason.

Video guide for this method: https://youtu.be/PQzxifFqMsA

</details>

## Credits

EWR would not exist without the people who reverse-engineered these protocols first. The database is assembled by an automated pipeline (`scripts/build_db.py`) that merges four upstream projects around the curated entries in this repository:

* **[reinkpy](https://codeberg.org/atufi/reinkpy)** - the largest of the four, and the backbone of the model coverage.
* **[ez-reset](https://github.com/CiRIP/ez-reset)** - per-counter byte maps and service limits, and the firmware commit step.
* **[reink](https://github.com/lion-simba/reink)** - the original protocol work on the older Stylus generation.
* **[Gutenprint](https://gutenprint.sourceforge.net/)** - model names and detection aliases.

## Support

EWR is free and will stay free. If it saved you a paid reset key or a trip to the repair shop, you can support development:

- USDT (TRC-20): `TXjUX9gCDjRUDU3L3HB3vfXbPdnTXiPzoR`
- BTC: `1Ep5bY7xBaJpWnzEG4PCjq9hTEjms9J1SR`

Not into donating? A ⭐ or a note in Discussions about which model worked helps just as much.

## ⚠️ Disclaimer
Manipulating hardware via raw USB packets carries inherent risks. EWR is provided "as is" without warranty of any kind. By using this software, you accept full responsibility for your hardware.
