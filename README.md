# EWR (Epson Waste Reset)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue)
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
* **Smart Protocol Engine:** Constructs exact EEPROM write packets (`|B`) on the fly for your specific model. It manages the IEEE 1284.4 (D4) credit system properly, so nothing overflows a buffer or locks the printer up.
* **~1450 printers**, from Stylus Photo R-series to the current ET / L / XP / WF lines. Cartridge ink levels can be reset too on models that expose a per-colour map.
* **OTA Database Sync:** The printer database refreshes itself in the background and swaps in on exit, so the database in use never changes underneath a running reset. The full database also ships in the download - EWR works offline out of the box.
* **Cross-Platform Core:**
  * **Windows:** Native Win32 `SetupAPI` with asynchronous `OVERLAPPED` I/O to safely drain the Windows Print Spooler buffers, plus a statically linked `libusb` for the vendor-specific interfaces `usbprint.sys` cannot reach - on ET-2xxx units the maintenance engine lives on one of those. Both transports run in one pass, `usbprint.sys` first. Zero custom drivers required.
  * **Linux & macOS:** Uses `libusb` to automatically detach the kernel driver (CUPS) for exclusive, raw hardware access.
* **Zero Hardcoded PIDs:** Scans your OS USB tree to find connected Epson printers and identifies them from the IEEE 1284 device ID they report.
* **A trace log for every run.** `ewr_trace.log` records every byte in both directions next to the binary. It is what makes a bug report solvable.
* **Replay Fallback:** If your printer is too new for the database, EWR can still parse and execute a raw Wireshark dump you supply (stripping USBPcap headers automatically).

## Usage

1. Download the latest version from the [Releases page](https://github.com/RxNaison/Epson-Waste-Reset/releases/latest) (or click the big green button above) and unzip it anywhere. Keep `ewr` and `database.json` together.
2. Turn your Epson printer on and connect it **via USB**. Network connections are not supported.
3. Run it:
   * **Windows:** double-click `ewr.exe`
   * **Linux / macOS:** `sudo ./ewr` *(raw USB access requires root)*
4. EWR detects the printer and offers the matching database entry. Press Enter to accept it, or type part of a model name to search for another.
5. It reads the printer's status and current counter values and prints them, then asks for confirmation once. Answer `y`.
6. It writes, then reads the counters back and tells you whether every one now holds its reset value.
7. **Turn the printer off and back on with its physical power button** to commit the change.

No internet connection is needed - the database ships in the download. When EWR can reach GitHub it quietly picks up new models in the background.

### Command-line options

Running with no options is the supported path. These exist for diagnosis and for people packaging EWR into something larger.

| Option | What it does |
| --- | --- |
| `--status`, `-s` | Read-only: printer status, ink levels and waste counter values. Sends no writes. |
| `--list`, `-l` | List every Epson USB interface with its IEEE 1284 device ID and database match, then exit. Read-only. |
| `--model <name>` | Skip the menu. Takes the exact name, an alias, or a unique fragment (`--model ET-2803`). |
| `--interface <n>` | Pin the run to interface `<n>` from `--list` and disable the automatic fallback. |
| `--dry-run` | Detect, read, and show exactly what a reset *would* write - then stop. |
| `--dump` | Read the EEPROM into a timestamped file. Dump twice around a change and diff to map an unknown printer. |
| `--find-addresses` | For a model with no reset addresses yet: read the EEPROM three times, asking you to run a head cleaning between reads, and report the bytes that rise every time. Read-only. |
| `--no-update` | Fully offline: no update check, no download, no staged swap on exit. Use it while editing `database.json`. |
| `--usb-soft-reset` | Diagnostic only, Windows. Off by default because it stalls the next write on ET-2xxx units. |
| `--help`, `-h` | The same list, from the binary. |

## When something goes wrong

Every run writes **`ewr_trace.log`** next to the executable, containing every packet sent and received. If EWR fails, or claims success and the error comes back after a power cycle, that file is what makes the problem solvable - please attach it to an issue rather than pasting the console output alone.

Useful first steps:

* `ewr --list` shows every USB interface EWR can see and which one it would pick. On a printer that also scans, several will be listed.
* `ewr --status` reads the counters without writing anything, so it is always safe to run.
* On Windows, close Epson Status Monitor from the system tray if the device reports as busy.
* If the printer says the **maintenance box** needs replacing rather than the ink pads, that box has its own chip - replace it or use a physical chip resetter. EWR resets the printer's internal counter, which is a different thing.

## Building from Source

### Prerequisites
* **Windows:** Visual Studio with MSVC C++ build tools. `libusb` is fetched and built by CMake, so the first configure needs `git` and a network connection; it is then linked statically and `ewr.exe` ships alone.
* **Linux (Arch/Debian):** `cmake`, `gcc`, `pkgconf`, `libusb-1.0-dev`, and `libcurl4-openssl-dev`.
* **macOS:** `brew install cmake libusb curl pkgconf`

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

On macOS the downloaded binary is quarantined by Gatekeeper. Clear it with `xattr -d com.apple.quarantine ewr`, or right-click the binary and choose **Open**.

## 🤝 Contributing a New Printer Model

Adding or fixing a printer means **editing `database.json` and opening a pull request** - no C++ required. `database.json` is the curated source of truth: the weekly rebuild merges upstream sources *around* your values and can never overwrite a hand edit. See **[CONTRIBUTING.md](CONTRIBUTING.md)** for the field reference and what makes a good pull request.

If your model is missing entirely, its addresses have to be found. Start with the safe method - EWR ships the tooling for it, and it never writes anything.

### Finding the counters yourself (read-only)

Around 113 database entries carry a working read key but no reset addresses - EWR can read those printers, it just does not know which bytes hold the waste counter. They appear in the menu as `(no reset addresses yet - read-only)`.

`--find-addresses` goes looking. It reads the EEPROM three times and asks you to run a head cleaning from the printer's own panel between reads, because a cleaning is the one action guaranteed to move the waste counter:

```
ewr --model ET-M1180 --find-addresses --no-update
```

Bytes that move the **same way after both** cleanings are reported, rising or falling, with their three readings side by side; a byte that moves only once is a timer or a page count and is dropped. A waste counter only ever rises, so the rising rows are the candidates and a falling one is something else - an ink level, a countdown. Two intervals is the minimum that tells those apart, which is why it takes three reads. Little-endian pairs are handled, so a low byte rolling over into its neighbour is reported as one 16-bit counter rather than discarded for falling. Nothing is written to the printer at any point, and the result is saved as `<Model>-found-addresses.json`.

Budget around twenty minutes: a full read of the low page takes a few minutes on older models, and you run it three times with two cleanings in between. A cleaning also uses real ink and pushes the waste counter up.

**This records readings, not a reset plan.** The saved file's `pad_groups` is empty on purpose: which of these is a waste counter, and what it resets to, is a judgement no dump can make - some pad groups reset certain bytes to `0x5E` (94) rather than 0. Open an issue or a pull request with the file rather than writing anything back blind.

If you would rather do it by hand, `--dump` writes a single timestamped snapshot you can `diff` yourself:

```
ewr --model L3150 --dump --no-update
diff ewr_dump_L3150_1736900000.txt ewr_dump_L3150_1736903600.txt
```

If every value comes back as `--`, the read key is wrong for your printer - try a different sibling from the same model line.

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
* **[ez-reset](https://github.com/CiRIP/ez-reset)** - per-counter byte maps and service limits, the firmware commit step, and the recovery channels.
* **[reink](https://github.com/lion-simba/reink)** - the original protocol work on the older Stylus generation.
* **[Gutenprint](https://gutenprint.sourceforge.net/)** - model names and detection aliases.

## Support

EWR is free and will stay free. If it saved you a paid reset key or a trip to the repair shop, you can support development:

- USDT (TRC-20): `TXjUX9gCDjRUDU3L3HB3vfXbPdnTXiPzoR`
- BTC: `1Ep5bY7xBaJpWnzEG4PCjq9hTEjms9J1SR`

Not into donating? A ⭐ or a note in Discussions about which model worked helps just as much.

## ⚠️ Disclaimer
Manipulating hardware via raw USB packets carries inherent risks. EWR is provided "as is" without warranty of any kind. By using this software, you accept full responsibility for your hardware.
