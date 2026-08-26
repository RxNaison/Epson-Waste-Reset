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
* **Smart Protocol Engine:** Constructs exact EEPROM write packets (`|B`) on the fly based on specific printer models. It safely manages the IEEE 1284.4 (D4) hardware credit system to prevent buffer overflows and lockups.
* **OTA Database Sync:** Automatically fetches a massive, continuously updated database of printer offsets and keys on startup using native OS APIs (Zero bloat).
* **Cross-Platform Core:**
  * **Windows:** Uses 100% native Win32 `SetupAPI` and robust Asynchronous `OVERLAPPED` I/O to safely drain the Windows Print Spooler buffers. Zero custom drivers required.
  * **Linux & macOS:** Uses `libusb` to automatically detach the kernel driver (CUPS) for exclusive, raw hardware access.
* **Zero Hardcoded PIDs:** Automatically scans your OS USB tree to find connected Epson printers.
* **Replay Fallback:** If your printer is brand new and not in the database yet, EWR can still dynamically parse and execute raw Wireshark dumps (stripping USBPcap headers automatically).

### Prerequisites (For Building from Source)
* **Windows:** Visual Studio with MSVC C++ build tools.
* **Linux (Arch/Debian):** `cmake`, `gcc`, `pkgconf`, `libusb-1.0-dev`, and `libcurl4-openssl-dev`.
* **macOS:** `brew install cmake libusb curl pkgconf`

## Usage

1. Download the latest version from the [Releases page](https://github.com/RxNaison/Epson-Waste-Reset/releases/latest) (or click the big green button above) and unzip it anywhere.
2. Ensure your Epson printer is turned on and connected to your computer via USB.
3. Run the executable:
   * **Windows:** Double-click `ewr.exe`
   * **Linux / macOS:** `sudo ./ewr` *(Raw USB access requires root)*
4. **Note:** On the very first run, EWR requires an internet connection to download the latest printer database. Afterward, it works entirely offline.
5. Type the number corresponding to your printer and hit Enter.
6. Wait for the `SUCCESS` message, then **turn your printer off and back on using its physical power button** to commit the EEPROM changes to the motherboard.

## Building from Source

Open your terminal in the root of the repository and run:

```bash
# 1. Generate the build files
cmake -B build

# 2. Compile the project (Release mode)
cmake --build build --config Release
```
The compiled executable `(ewr.exe or ewr)` will be placed in the repository root (the build pins the output directory there so the binary sits next to `database.json` and the `models/` folder).

## 🤝 Contributing a New Printer Model (Replay Fallback)

If your printer isn't in the database yet, you can still add support for it using our Replay method without writing a single line of code!

### Step 1: Capture the Hardware Conversation
1. Install [Wireshark](https://www.wireshark.org/) (Ensure **USBPcap** is installed on Windows) on your VM
2. Connect your printer to the PC and turn it on
3. Connect your printer to the VM
4. Open Wireshark and start capturing on your USB interface
5. Open the sketchy Epson adjustment program you found on the internet inside the VM (this keeps your host machine safe from potential malware)
6. Run the "Reset Waste Counters" command
7. Stop the Wireshark capture immediately after the program says shutdown the printer

### Step 2: Export the Payloads
1. In Wireshark, type this exact filter into the display filter bar and hit Enter:
   `usb.endpoint_address.direction == 0 && usb.transfer_type != 0x02`
   *(This isolates the `URB_BULK out` packets sent to the printer).*
2. Go to **File** -> **Export Packet Dissections** -> **As C Arrays...**
3. Save the file with your printer's model name (e.g., `L3150.c`).

### Step 3: Test and Open a Pull Request
1. Drop your new `L3150.c` file into your local EWR `models/` folder.
2. Run EWR. The parser will automatically strip the Wireshark metadata and execute the payload.
3. If your waste counter successfully resets, open a Pull Request and upload your `.c` file to the repository so the rest of the world can use it!

Video Guide: https://youtu.be/PQzxifFqMsA

## Credits
Special thanks to the [reinkpy](https://codeberg.org/atufi/reinkpy) project for their fantastic database. EWR uses an automated GitHub Actions pipeline to sync and convert their TOML database into our C++ backend, merging their massive printer support with our standalone C++ execution environment.

## ⚠️ Disclaimer
Manipulating hardware via raw USB packets carries inherent risks. EWR is provided "as is" without warranty of any kind. By using this software, you accept full responsibility for your hardware.
