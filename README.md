# rtw88-win95

**Native WDM USB Wi-Fi Driver & Hardware Probe for Realtek RTL8822BU on Windows 95 OSR 2.1 / 2.5**

![Platform](https://img.shields.io/badge/Platform-Windows%2095%20OSR%202.1%20%2F%202.5-blue.svg)
![Target Hardware](https://img.shields.io/badge/Hardware-Realtek%20RTL8822BU%20(802.11ac)-orange.svg)
![Toolchain](https://img.shields.io/badge/Toolchain-MinGW--w64%20(i686)-green.svg)
![Status](https://img.shields.io/badge/Status-Milestone%202%20Complete-brightgreen.svg)

`rtw88-win95` is an open-source project bringing modern dual-band **Realtek RTL8822BU (802.11ac/n/g/b)** USB Wi-Fi dongles (such as the popular Nineplus USB Wi-Fi adapter) to vintage retro PCs running **Windows 95**.

---

## 🎯 Verified Target Hardware

Tested and verified on real vintage iron:
- **Computer**: Toshiba Satellite 320CDS (Intel Pentium MMX 233 MHz, 32 MB RAM)
- **Operating System**: Windows 95 OSR 2.1 (4.00.950 B) with Microsoft USB Supplement (`USBSUPP.EXE` / `NTKERN.VXD`)
- **USB Host Controller**: NEC PCI to USB Open Host Controller (OHCI, `PCI\VEN_1033&DEV_0035`)
- **Wi-Fi Adapter**: Nineplus USB Wi-Fi Dongle (`USB\VID_0BDA&PID_B812`, Realtek RTL8822BU)

---

## 🔬 Live Silicon Verification Output

```
======================================================
SUCCESS! Connected to Nineplus USB Wi-Fi (Realtek 8822BU)!
------------------------------------------------------
--- Core Registers ---
REG_SYS_FUNC_EN  (0x0002) = 0x501C  [OK]  (MAC, Baseband, and USB PHY powered)
REG_SYS_CLK_CTRL (0x0008) = 0x3023  [OK]  (System clocks & PLL active)
REG_SYS_CFG1     (0x00F0) = 0x0C493D35  [OK]  (RTL8822B Silicon Stepping Confirmed)

--- Hardware MAC Address ---
MAC (Port 0 Reg 0x0610):  EA:EA:EA:EA:EA:EA  [OK]  (Power-on hardware default state)
MAC (Station Reg 0x0050): 00:00:00:00:02:0E  [OK]

Milestone 2 MAC Identification Complete!
======================================================
```

---

## 🛠️ Windows 95 WDM Architectural Quirks Solved

Running modern WDM kernel-mode drivers under Windows 95's early `NTKERN.VXD` wrapper involves navigating several undocumented pitfalls:

1. **Strict Symbol Import Whitelist**:
   Windows 95 `NTKERN.VXD` only emulates a small subset of NT 4.0 / WDM kernel routines. Common modern routines like `PoCallDriver`, `PoStartNextPowerIrp`, `ZwWriteFile`, and standard CRT exports like `strlen` are completely absent and will prevent driver loading (Code 28 / Code 10). `RTWPROBE.SYS` was audited to use strictly verified `ntoskrnl.exe` exports:
   - `IoAttachDeviceToDeviceStack`, `IoBuildDeviceIoControlRequest`, `IoCreateDevice`, `IoCreateSymbolicLink`, `IoDeleteDevice`, `IoDeleteSymbolicLink`, `IoDetachDevice`, `IofCallDriver`, `IofCompleteRequest`, `KeInitializeEvent`, `KeSetEvent`, `KeWaitForSingleObject`, `RtlInitUnicodeString`, `DbgPrint`.

2. **Bug Check 0x36 (`DEVICE_REFERENCE_COUNT_NOT_ZERO`) on Unplug**:
   In Windows 95, `IoDeleteDevice` strictly tests:
   ```asm
   cmpl $0x0, 0x4(%esi)   ; DeviceObject->ReferenceCount
   jne  bugcheck          ; KeBugCheckEx(0x36) if non-zero!
   ```
   During surprise removal or unplugging, the PnP manager holds a reference during `IRP_MN_REMOVE_DEVICE`. Calling `IoDeleteDevice` while `ReferenceCount != 0` immediately halts the system. `RTWPROBE.SYS` prevents this by guarding deletion and detaching cleanly.

3. **PnP StartDevice Watchdog (Code 10)**:
   Lengthy hardware interrogation loops (such as eFuse reads) during `IRP_MN_START_DEVICE` trip Windows 95's 5-second PnP watchdog, causing false Code 10 errors. `RTWPROBE.SYS` returns `STATUS_SUCCESS` instantly on `START_DEVICE` and executes full register/MAC queries on-demand via `DeviceIoControl`.

4. **Dynamic Hardware Instance Binding (`AUTOBIND.EXE`)**:
   Windows 95 assigns dynamic instance strings under `HKLM\Enum\USB\VID_0BDA&PID_B812` (e.g. `1USB&ROOT_HUB...`, `2USB...`). `AUTOBIND.EXE` automatically inspects the registry, reserves an isolated class index (`USB\0005`), binds `*ntkern` and `RTWPROBE.SYS`, and clears `ConfigFlags = 0`.

---

## 📦 Project Contents

- **`rtwprobe.c`**: Kernel-mode WDM probe driver.
- **`rtwtest.c`**: Pure Win32 user-mode diagnostics tool (zero runtime dependencies, runs on raw Windows 95).
- **`autobind.c`**: Pure Win32 1-click registry binder.
- **`rtwprobe.inf`**: Windows 95 PnP INF installation file.
- **`rtw8822b_fw.bin`**: Official Realtek RTL8822BU microcode firmware image (161 KB).
- **`Makefile`**: Multi-target build script for `i686-w64-mingw32-gcc`.

---

## 🚀 Quickstart Guide

1. Download or copy `RTWPROBE.SYS`, `RTWTEST.EXE`, and `AUTOBIND.EXE` to your Windows 95 machine.
2. Copy `RTWPROBE.SYS` to `C:\WINDOWS\SYSTEM32\DRIVERS\RTWPROBE.SYS` (and `C:\WINDOWS\SYSTEM\`).
3. Plug in your RTL8822BU USB Wi-Fi dongle.
4. Run **`AUTOBIND.EXE`** to automatically register the driver binding.
5. In Device Manager, click **Refresh** (or unplug and replug the dongle).
6. Run **`RTWTEST.EXE`** to verify communication!

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

### Manual Compilation Commands:

```bash
# Compile RTWPROBE.SYS (WDM Driver)
i686-w64-mingw32-gcc -O2 -s -shared -Wl,--subsystem,native -Wl,--entry,_DriverEntry@8 \
  -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--image-base,0x10000 \
  -nostartfiles -o RTWPROBE.SYS rtwprobe.c -lntoskrnl -lusbd

# Compile RTWTEST.EXE (Win32 Diagnostic Tool)
i686-w64-mingw32-gcc -O2 -s -nostdlib -e _WinMain@16 \
  -o RTWTEST.EXE rtwtest.c -lkernel32 -luser32

# Compile AUTOBIND.EXE (Registry Binding Utility)
i686-w64-mingw32-gcc -O2 -s -nostdlib -e _WinMain@16 \
  -o AUTOBIND.EXE autobind.c -lkernel32 -luser32 -ladvapi32
```

---

## 🗺️ Roadmap

- [x] **Milestone 1: Endpoint 0 Control Transfer Engine**
  - Implement URB-based vendor control transfers (`bRequest = 0x05`, `bmRequestType = 0xC0`).
  - Read core hardware registers (`REG_SYS_FUNC_EN`, `REG_SYS_CLK_CTRL`, `REG_SYS_CFG1`).
- [x] **Milestone 2: Hardware MAC & eFuse Identification**
  - Read MAC Port 0 (`0x0610`) and Station (`0x0050`) registers.
  - Implement safe unplugging without Bug Check 0x36.
  - Automated dynamic registry binding tool (`AUTOBIND.EXE`).
- [ ] **Milestone 3: Firmware Upload & CPU Startup**
  - Stream `rtw8822b_fw.bin` into chip SRAM across USB vendor transfers.
  - Initialize 32-bit RISC WLAN CPU (`REG_MCUFW_CTRL`).
- [ ] **Milestone 4: NDIS 3.1 / 5.0 Miniport & TCP/IP Binding**
  - Bind to Windows 95 network subsystem.
  - 802.11 beacon scanning and WPA/WPA2-PSK or open Wi-Fi association.

---

## 📄 License

GPL-2.0 / BSD-3-Clause

