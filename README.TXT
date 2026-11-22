============================================================
Realtek RTL8822BU (VID_0BDA&PID_B812) USB Probe Driver (Win95)
============================================================

What This Does:
---------------
This is Milestone 1: A minimal WDM probe driver built specifically
for Windows 95 (with USB Supplement / NTKERN) that binds to the
Nineplus USB Wi-Fi dongle (VID_0BDA&PID_B812).

When loaded, it sends USB Vendor Control Requests (bRequest 0x05)
to read three Realtek hardware registers:
  1. REG_SYS_FUNC_EN  (0x0002) - 16-bit
  2. REG_SYS_CLK_CTRL (0x0008) - 16-bit
  3. REG_SYS_CFG1     (0x00F0) - 32-bit

Results are written automatically to C:\RTWLOG.TXT!

How to Install on Toshiba 320CDS:
---------------------------------
1. Copy the RTWPROBE folder to C:\RTWPROBE on your Toshiba.
2. Plug in the USB Wi-Fi dongle.
3. Windows 95 will either show "Update Device Driver Wizard" or
   you can go to:
     My Computer -> Properties -> Device Manager
     Find the "Unknown Device" (or yellow "?") under USB or Other Devices.
     Click Properties -> Driver -> Update Driver.
4. Tell Windows to search for a driver, and click "Browse" / "Have Disk...".
5. Point it to C:\RTWPROBE (where RTWPROBE.INF is located).
6. Windows 95 will copy RTWPROBE.SYS to C:\WINDOWS\SYSTEM32\DRIVERS
   and start the driver!

How to Check the Output:
------------------------
Open C:\RTWLOG.TXT in Notepad!
You should see:
  RTWPROBE: Realtek RTL8822BU (VID_0BDA&PID_B812) Loaded!
  RTWPROBE: Probing hardware registers via USB Vendor Requests...
  RTWPROBE: SUCCESS! Read REG_SYS_FUNC_EN (0x0002) = 0x...
  RTWPROBE: SUCCESS! Read REG_SYS_CLK_CTRL (0x0008) = 0x...
  RTWPROBE: SUCCESS! Read REG_SYS_CFG1 (0x00F0) = 0x...
  RTWPROBE: Milestone 1 Probe Complete!
