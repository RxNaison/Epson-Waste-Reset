# EWR (Epson Waste Reset)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue)
![C++](https://img.shields.io/badge/language-C++17-orange)
![License](https://img.shields.io/badge/license-Apache_License_2.0-green)

A free, cross-platform, and completely open-source C++ utility to reset the "Waste Ink Pad" counter on Epson printers. 

EWR bypasses the need to pay for sketchy third-party reset keys (like WIC Reset) or run malicious, virus-flagged `AdjProg.exe` binaries. By utilizing raw hardware state-machine replaying, EWR communicates directly with the printer's motherboard over USB to safely zero out the EEPROM waste counters.

## Features
* **Cross-Platform Core:**
  * **Windows:** Uses 100% native Win32 `SetupAPI` and robust Asynchronous `OVERLAPPED` I/O to safely drain the Windows Print Spooler buffers. Zero custom drivers required.
  * **Linux:** Uses `libusb` to automatically detach the kernel driver (CUPS) for exclusive, raw hardware access.
* **Smart Payload Parser:** EWR builds its executable payloads dynamically from raw Wireshark dumps. It automatically detects and strips `USBPcap` kernel metadata headers, meaning contributors never have to manually edit hex arrays.
* **Zero Hardcoded PIDs:** Automatically scans your OS USB tree to find connected Epson printers.

### Prerequisites
* **Windows:** Visual Studio with MSVC C++ build tools.
* **Linux (Arch/Debian):** `cmake`, `gcc`, `pkgconf`, and `libusb-1.0-dev`.

## Usage

1. Ensure your Epson printer is turned on and connected via USB.
2. Check the `models/` folder to see if your printer model (e.g., `L222.txt`) is already supported.
3. Run the executable:
   * **Windows:** Double-click `ewr.exe`
   * **Linux:** `sudo ./ewr` *(Raw USB access requires root)*
4. Type the number corresponding to your printer and hit Enter.
5. Wait for the `SUCCESS` message, then **turn your printer off and back on using its physical power button** to commit the EEPROM changes to the motherboard.

## Building from Source

### Build Instructions
Open your terminal in the root of the repository and run:

```bash
# 1. Generate the build files
cmake -B build

# 2. Compile the project (Release mode)
cmake --build build --config Release
```
The compiled executable `(ewr.exe or ewr)` will be located in the `Release` directory.

## 🤝 Contributing a New Printer Model

If your printer isn't in the `models/` folder yet, you can add support for it without writing a single line of code!

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
2. Run EWR. The Smart Parser will automatically strip the Wireshark metadata and execute the payload.
3. If your waste counter successfully resets, open a Pull Request and upload your `.c` file to the repository so the rest of the world can use it!

## ⚠️ Disclaimer
Manipulating hardware via raw USB packets carries inherent risks. EWR is provided "as is" without warranty of any kind. By using this software, you accept full responsibility for your hardware.
