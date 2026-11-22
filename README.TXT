============================================================
Realtek RTL8822BU (VID_0BDA&PID_B812) Driver Suite for Windows 95
============================================================

Verified on authentic vintage hardware:
Toshiba Satellite 320CDS (Pentium MMX 233 MHz, 32 MB RAM)
Windows 95 OSR 2.1 / 2.5 with USB Supplement (USBSUPP / NTKERN)

Contents of this Package:
-------------------------
1. RTW88.SYS     - Milestone 4.2: Native NDIS 3.1 Network Miniport Driver
2. RTW88.INF     - Network installation INF (Class=Net, binds to VTCP.386)
3. RTWPROBE.SYS  - WDM Hardware Diagnostic & Microcode Flasher Driver
4. RTWPROBE.INF  - WDM Device installation INF (Class=USB)
5. AUTOBIND.EXE  - 1-Click GUI Driver Mode Switcher:
                   [Yes]    = Bind RTW88.SYS (NDIS 3.1 TCP/IP Networking)
                   [No]     = Bind RTWPROBE.SYS (Hardware Diagnostics)
                   [Cancel] = Abort
6. RTWSCAN.EXE   - USB Bulk IN engine & 802.11 beacon parser
7. RTWTEST.EXE   - Milestone 2/3 hardware register & eFuse MAC verifier
8. rtw8822b_fw.bin - Realtek RTL8822BU RISC microcode firmware (160 KB)

Key Fixes in this Release:
--------------------------
1. KeBugCheckEx (0x36 / Error code 3) Fixed on Unplug:
   - Handled IRP_MJ_CLEANUP in DriverEntry.
   - Guarded IoDeleteDevice against non-zero ReferenceCount during PnP removal.
   - Defer final FDO deletion until IRP_MJ_CLOSE when user handles close.
   - USB disconnects / unplugs now occur safely with ZERO system crashes.

2. Milestone 4.2 NDIS 3.1 Network Driver:
   - Built with strictly 0 CMOV instructions (safe for Pentium MMX).
   - Links cleanly to NDIS.SYS (NdisInitializeWrapper, NdisMRegisterMiniport).
   - Reports standard 802.3 Ethernet medium (NdisMedium802_3).
   - Reports hardware eFuse MAC: 80:42:EF:68:80:42.
   - Answers standard Ethernet OIDs for Windows 95 TCP/IP stack (VTCP.386).

How to Install for Networking (Milestone 4.2):
----------------------------------------------
Option A (Automatic):
  1. Copy RTW88.SYS to C:\WINDOWS\SYSTEM\
  2. Run AUTOBIND.EXE and click [Yes] (Bind RTW88.SYS for TCP/IP).
  3. In Device Manager, click "Refresh" (or unplug and replug the Wi-Fi dongle).

Option B (Network Control Panel):
  1. Open Control Panel -> Network.
  2. Click "Add..." -> "Adapter" -> "Have Disk...".
  3. Point to this folder (where RTW88.INF is located).
  4. Select "Realtek RTL8822BU Wireless LAN 802.11ac Adapter".
  5. Windows 95 will bind TCP/IP (VTCP.386) automatically!

How to Run Diagnostics / Scanner:
---------------------------------
1. Run AUTOBIND.EXE and click [No] (Bind RTWPROBE.SYS).
2. Unplug/replug the Wi-Fi dongle or click Refresh in Device Manager.
3. Run RTWTEST.EXE to verify firmware boot (REG_MCUFW_CTRL = 0xA280) and eFuse MAC.
4. Run RTWSCAN.EXE to exercise the USB Bulk polling engine.
