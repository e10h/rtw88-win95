# rtw88-win95

**Native WDM USB Wi-Fi Driver & Hardware Probe for Realtek RTL8822BU on Windows 95 OSR 2.1 / 2.5**

![Platform](https://img.shields.io/badge/Platform-Windows%2095%20OSR%202.1%20%2F%202.5-blue.svg)
![Target Hardware](https://img.shields.io/badge/Hardware-Realtek%20RTL8822BU%20(802.11ac)-orange.svg)
![Toolchain](https://img.shields.io/badge/Toolchain-MinGW--w64%20(i686)-green.svg)
![Status](https://img.shields.io/badge/Status-Milestone%201%20Complete-brightgreen.svg)

`rtw88-win95` is an open-source project bringing modern dual-band **Realtek RTL8822BU (802.11ac/n/g/b)** USB Wi-Fi dongles (such as the popular Nineplus USB Wi-Fi adapter) to vintage retro PCs running **Windows 95**.

---

## 🎯 Target Hardware

Tested on real vintage hardware:
- **Computer**: Toshiba Satellite 320CDS (Intel Pentium MMX 233 MHz, 32 MB RAM)
- **Operating System**: Windows 95 OSR 2.1 (4.00.950 B) with Microsoft USB Supplement (`USBSUPP.EXE` / `NTKERN.VXD`)
- **USB Host Controller**: NEC PCI to USB Open Host Controller (OHCI, `PCI\VEN_1033&DEV_0035`)
- **Wi-Fi Adapter**: Nineplus USB Wi-Fi Dongle (`USB\VID_0BDA&PID_B812`, Realtek RTL8822BU)

---

## 🔬 Silicon Verification Output

```
======================================================
SUCCESS! Connected to Nineplus USB Wi-Fi (Realtek 8822BU)!
------------------------------------------------------
--- Core Registers ---
REG_SYS_FUNC_EN  (0x0002) = 0x501C  [OK]  (MAC, Baseband, and USB PHY powered)
REG_SYS_CLK_CTRL (0x0008) = 0x3023  [OK]  (System clocks & PLL active)
REG_SYS_CFG1     (0x00F0) = 0x0C493D35  [OK]  (RTL8822B Silicon Stepping Confirmed)

Milestone 1 USB Control Transfer Engine Complete!
======================================================
```

---

## 📦 Project Contents

- **`rtwprobe.c`**: Kernel-mode WDM probe driver.
- **`rtwtest.c`**: Pure Win32 user-mode diagnostics tool (zero runtime dependencies, runs on raw Windows 95).
- **`rtwprobe.inf`**: Windows 95 PnP INF installation file.
- **`Makefile`**: Multi-target build script for `i686-w64-mingw32-gcc`.

---

## 🔨 Building from Source

Build requires **MinGW-w64** (`i686-w64-mingw32-gcc`):

```bash
# Clone the repository
git clone https://github.com/e10h/rtw88-win95.git
cd rtw88-win95

# Build all binaries
make
```

---

## 🗺️ Roadmap

- [x] **Milestone 1: Endpoint 0 Control Transfer Engine**
  - Implement URB-based vendor control transfers (`bRequest = 0x05`, `bmRequestType = 0xC0`).
  - Read core hardware registers (`REG_SYS_FUNC_EN`, `REG_SYS_CLK_CTRL`, `REG_SYS_CFG1`).
- [ ] **Milestone 2: Hardware MAC & eFuse Identification**
- [ ] **Milestone 3: Firmware Upload & CPU Startup**
- [ ] **Milestone 4: NDIS 3.1 / 5.0 Miniport & TCP/IP Binding**

---

## 📄 License

GPL-2.0 / BSD-3-Clause
