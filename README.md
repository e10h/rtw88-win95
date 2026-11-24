# rtw88-win95

**Open-Source WPA2-PSK (AES-CCMP) Wi-Fi Driver Suite for Realtek RTL8822BU USB Dongles on Windows 95 OSR 2.1 / 2.5**

![Platform](https://img.shields.io/badge/Platform-Windows%2095%20OSR%202.1%20%2F%202.5-blue.svg)
![Target Hardware](https://img.shields.io/badge/Hardware-Realtek%20RTL8822BU%20(802.11ac)-orange.svg)
![Toolchain](https://img.shields.io/badge/Toolchain-MinGW--w64%20(i686)%20%2B%20JWasm%2FJWlink-green.svg)
![Status](https://img.shields.io/badge/Status-Live%20WPA2%20Wi--Fi%20%26%20Web%20Browsing%20Working!-brightgreen.svg)

`rtw88-win95` brings modern **Realtek RTL8822BU (`USB\VID_0BDA&PID_B812`)** USB Wi-Fi dongles — complete with **WPA2-PSK (PBKDF2-SHA1 + 4-Way Handshake + AES-128-CCMP)**, native Windows 95 `VTCP.386` TCP/IP integration, DNS, ping, and live web browsing — to authentic vintage PCs running **Windows 95**.

---

## 🎯 Verified Target Hardware

Tested and verified end-to-end on real vintage iron (live WPA2-PSK Wi-Fi, ~28 ms LAN ping, `ping 1.1.1.1`, `ping google.com`, and live web browsing in Internet Explorer):
- **Computer**: Toshiba Satellite 320CDS (Intel Pentium MMX 233 MHz, 32 MB RAM)
- **Operating System**: Windows 95 OSR 2.1 (4.00.950 B) with Microsoft USB Supplement (`USBSUPP.EXE` / `NTKERN.VXD`)
- **USB Host Controller**: NEC PCI to USB Open Host Controller (OHCI 1.1, `PCI\VEN_1033&DEV_0035`)
- **Wi-Fi Adapter**: Nineplus 802.11ac USB Wi-Fi Dongle (`USB\VID_0BDA&PID_B812`, Realtek RTL8822BU)

---

## 🏗️ Dual-Driver Architecture

Windows 95's early `NTKERN.VXD` WDM layer cannot bind a WDM USB driver directly to the native Ring-0 NDIS 3.1 VxD TCP/IP stack (`VTCP.386`). `rtw88-win95` solves this cleanly with three cooperating components (all compiled strictly Pentium MMX safe with **0 `CMOV` instructions**):

```
[Internet Explorer / ping / Winsock]
               │
        [VTCP.386 / NDIS.VXD]
               │
        [RTW88.VXD]          (Native Win95 LE NDIS 3.1 VxD, ROOT\NET\RTW88)
               │  DeviceIoControl (DIOC 1..4)
        [RTWBRIDGE.EXE]      (Native Win95 GUI: RTL8822BU Radio + 802.11 + WPA2-PSK AES-CCMP)
               │  DeviceIoControl (IOCTL 0x820..0x825)
        [RTWPROBE.SYS]       (WDM USB Driver, USB\VID_0BDA&PID_B812 via USBD.SYS / OHCI)
               │
    [Realtek RTL8822BU USB Dongle] ──► WPA2-PSK 802.11 Wi-Fi ──► Internet
```

---

## 📦 Project Contents

- **`rtwprobe.c` / `RTWPROBE.SYS`**: Minimal WDM USB driver (strictly 14 proven Win95 `ntoskrnl.exe` imports, RTL8822BU `0x04E0` USB erratum fix, batched register/RF SIPI I/O, raw Bulk IN/OUT pipe access, safe surprise removal without Bug Check `0x36`).
- **`rtw88_vxd.c` + `rtw88vxd.asm` / `RTW88.VXD`**: Native Windows 95 LE NDIS 3.1 VxD adapter bound to `VTCP.386` with Ring-0 TX/RX packet queues exposed via `W32_DEVICEIOCONTROL`.
- **`rtwbridge.c` + `wpa2_crypto.h` + `rtw8822b_tables.h` / `RTWBRIDGE.EXE`**: Native Windows 95 GUI Wi-Fi bridge (zero libc, zero background throttling). Handles RTL8822BU power-on, firmware upload (`rtw8822b_fw.bin`), MAC/BB/AGC/RF tables, 2.4 GHz scanning, 802.11 Auth/Assoc, self-contained WPA2-PSK 4-Way Handshake + RFC 3394 GTK unwrap + AES-128-CCMP encrypt/decrypt (with A-MSDU & QoS `+HTC` support), automatic TX rate probing (1M CCK / 6M OFDM), and live Ethernet↔802.11 bridging.
- **`rtwscan.c` / `RTWSCAN.EXE`**: Standalone 2.4 GHz Wi-Fi scanner and radio diagnostic utility.
- **`autobind.c` / `AUTOBIND.EXE`**: 1-click Windows 95 registry & driver installer for Dual Mode (`RTWPROBE.SYS` + `RTW88.VXD`).
- **`rtw8822b_fw.bin`**: Realtek RTL8822BU microcode firmware image (161 KB).
- **`Makefile`**: Multi-target build script (`i686-w64-mingw32-gcc`, `JWasm`, `JWlink`).

---

## 🛠️ Key Windows 95 & Silicon Quirks Solved

1. **Strict Win95 `NTKERN.VXD` Import Whitelist & Bug Check `0x36`**: Audited `RTWPROBE.SYS` down to 14 verified `ntoskrnl.exe` exports and guarded `IoDeleteDevice` during surprise removal.
2. **Pentium MMX Compatibility (0 `CMOV`s)**: All binaries compiled with `-march=pentium-mmx -mtune=pentium-mmx -mno-stack-arg-probe` to avoid i686 `#UD` faults.
3. **RTL8822BU Silicon Errata**: Implemented `0x04E0` always-on register bridge write, SIPI RF writes (`0x0C90`/`0x0E90`), USB 1.1 64-byte MaxPacketSize short-packet termination, and 4-byte 802.11 FCS stripping (`BIT_APP_FCS`).
4. **Win95 OHCI Single-Threaded USB + Native GUI**: Eliminated two-handle Bulk IN/OUT contention under Win95 `OPENHCI.SYS` and replaced the DOS console with a native `--subsystem,windows` GUI (`256 KB` upfront stack commit) so Windows 95 never suspends or throttles Wi-Fi in the background.

---

## 🚀 Quickstart Guide (Windows 95)

1. Extract the binaries (`RTWPROBE.SYS`, `RTW88.VXD`, `AUTOBIND.EXE`, `RTWBRIDGE.EXE`, `rtw8822b_fw.bin`) into a folder (e.g. `C:\RTWPROBE\`).
2. Plug in your RTL8822BU USB Wi-Fi dongle, run **`AUTOBIND.EXE`** (Dual Mode), and reboot once.
3. In **Control Panel → Network → TCP/IP -> Realtek...**, configure your IP, Gateway (e.g. `192.168.1.1`), and DNS (e.g. `1.1.1.1`) — or enable DHCP — and set Internet Explorer to **Local Area Network** (uncheck *Work Offline*).
4. Create **`WIFI.TXT`** next to `RTWBRIDGE.EXE`:
   ```
   YourWiFiSSID
   YourWiFiPassword
   ```
   *(Optional Line 3: channel number `1`–`11` for instant connection.)*
5. Double-click **`RTWBRIDGE.EXE`**, wait for **`CONNECTED`**, minimize it, and browse the web!

---

## 🔨 Building from Source

Requires **MinGW-w64** (`i686-w64-mingw32-gcc`), plus **JWasm / JWlink / Python 3** for rebuilding `RTW88.VXD`:

```bash
git clone https://github.com/e10h/rtw88-win95.git
cd rtw88-win95
make RTWBRIDGE.EXE   # Build native Win95 GUI Wi-Fi bridge
make RTWPROBE.SYS    # Build WDM USB driver
make RTWSCAN.EXE     # Build 2.4 GHz Wi-Fi scanner
```

---

## 🗺️ Milestones (All Complete & Verified on Real Iron!)

- [x] **Milestone 1–3**: WDM USB driver (`RTWPROBE.SYS`), register/eFuse access, 5 USB pipes, and RTL8822BU firmware boot (`0xC078`).
- [x] **Milestone 4–6**: Native Win95 LE NDIS 3.1 VxD (`RTW88.VXD`) bound to `VTCP.386` with live Ring-0 TX/RX bridging (`AUTOBIND.EXE` Dual Mode).
- [x] **Milestone 7**: Full RTL8822BU MAC/BB/AGC/RF radio bring-up & live 2.4 GHz Wi-Fi scanning (`RTWSCAN.EXE`, 0 CRC errors).
- [x] **Milestone 8**: Full 802.11 Auth/Assoc + WPA2-PSK 4-Way Handshake + software AES-128-CCMP (`wpa2_crypto.h`).
- [x] **Milestone 9**: Native Windows 95 GUI Ethernet↔802.11 bridge (`RTWBRIDGE.EXE`) — verified live LAN/internet pings (`192.168.1.1` @ ~28 ms, `1.1.1.1`, `google.com`) and live web browsing in Internet Explorer on Windows 95!

---

## 📄 License

GPL-2.0 / BSD-3-Clause

