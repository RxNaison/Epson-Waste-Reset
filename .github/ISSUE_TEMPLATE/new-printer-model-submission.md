---
name: New Printer Model Submission
about: Submit a working Wireshark dump to add support for a new Epson printer.
title: "[Model] Epson <Model Name> Payload Submission"
labels: enhancement, needs-verification, new-model
assignees: RxNaison

---

**Thank you for contributing to EWR!** 
To add your printer to the official database, please fill out the information below and attach your captured payload file.

**Printer Information:**
 * Exact Printer Model: [e.g., Epson L3150, EcoTank ET-2750]
 * Did this payload successfully reset your waste ink counter? [Yes / No]

**The Payload File:**
Please drag and drop your Wireshark export file (`.txt` or `.c`) directly into this text box. GitHub will automatically upload it. 
*(Note: Do not paste the raw hex text into the issue body, please upload the actual file).*

[Drag and drop your .txt or .c file here]

**Capture Environment:**
 * How did you capture this? [e.g., Official AdjProg on Windows 10 VM, WIC Reset on Native Windows 11]
 * Which EWR version did you use to test the replay? [e.g., v1.1.0-alpha]

**Pre-flight Checklist (Please check all before submitting):**
- [ ] I filtered the Wireshark capture strictly to `usb.endpoint_address.direction == 0 && usb.transfer_type != 0x02`.
- [ ] I have personally tested this payload using EWR, and it successfully reset my printer.
- [ ] I waited for the EWR "SUCCESS" message and restarted my printer physically.
- [ ] The attached file is in standard C Array format from Wireshark.

**Any notes or quirks about this specific printer?**
Add any extra details here (e.g., "This printer took 10 seconds to respond," or "I had to run the payload twice").
