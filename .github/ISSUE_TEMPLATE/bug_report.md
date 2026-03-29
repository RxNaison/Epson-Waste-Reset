---
name: Bug report
about: Create a report to help us improve
title: "[Bug] Epson <Model>: <Short description>."
labels: bug, hardware-issue, needs-triage
assignees: RxNaison

---

**Describe the bug**
A clear and concise description of what the bug is. (e.g., "The program hangs after sending packet 3," or "The printer carriage crashed into the wall.")
**Printer Information (CRITICAL):**
 * Printer Model: [e.g., Epson L3150]
 * Did you capture this payload yourself, or use an existing one in the models/ folder?
 * What are the physical printer lights doing? (e.g., Both red lights blinking)
**To Reproduce**
Steps to reproduce the behavior:
 * Connected printer via USB to [Native PC / VirtualBox / VMware]
 * Ran the executable in the terminal using command: ...
 * Selected printer payload number ...
 * See error
**Expected behavior**
A clear and concise description of what you expected to happen.
Console Output / Log
Please copy and paste the exact output from your terminal/command prompt.
(If the program crashed, paste the last few lines it printed before closing).
```bash
[Paste terminal output here]
```
**Desktop / Environment:**
 * OS: [e.g., Windows 11 Native, Arch Linux, Windows 10 VM on Mac]
 * EWR Version: [e.g., v1.1.0-alpha]
 * Connection: [e.g., Direct USB to motherboard, USB Hub, USB Passthrough to VM]

**Pre-flight Checklist (Please check all before submitting):**
- [ ] I am using the latest release of EWR.
- [ ] I have read the `README.md` and understand the hardware risks.
- [ ] I ran the program with the correct permissions (Administrator on Windows, `sudo` on Linux).
- [ ] My printer is physically powered on, not just plugged in.
- [ ] I have searched the existing issues to make sure this hasn't been reported already.

**Additional context**
Add any other context about the problem here (e.g., "I heard a grinding noise," or "I had to manually kill the terminal").
