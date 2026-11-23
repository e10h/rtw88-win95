CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -s -march=pentium-mmx -mtune=pentium-mmx -mno-stack-arg-probe
VXD_CFLAGS = -O2 -march=pentium-mmx -mtune=pentium-mmx -mno-stack-arg-probe \
             -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables \
             -fno-reorder-blocks-and-partition -fno-ident -fpack-struct=1

JWASM ?= /Users/ethan/.gemini/antigravity/brain/8dbb8657-600c-4d8e-9f0c-ae28412df3dc/scratch/tools/jwasm_bin
JWLINK ?= /Users/ethan/.gemini/antigravity/brain/8dbb8657-600c-4d8e-9f0c-ae28412df3dc/scratch/tools/jwlink_bin

all: RTWPROBE.SYS RTWTEST.EXE AUTOBIND.EXE RTWSCAN.EXE RTW88.VXD TESTVXD.VXD VXDDIAG.EXE

TESTVXD.VXD: testvxd.asm testvxd.lnk fixvxd.py
	$(JWASM) -coff -Cx -Fo testvxd.obj testvxd.asm
	$(JWLINK) @testvxd.lnk
	python3 fixvxd.py TESTVXD.VXD

VXDDIAG.EXE: vxddiag.c
	$(CC) $(CFLAGS) -nostdlib -e _WinMain@16 -o $@ $< -lkernel32 -luser32 -ladvapi32

RTW88.VXD: rtw88vxd.asm rtw88_vxd.c rtw88vxd.lnk fixvxd.py vxd/ddk/lib/libndis.clb
	$(JWASM) -coff -Cx -Fo rtw88vxd.obj rtw88vxd.asm
	$(CC) $(VXD_CFLAGS) -Wall -Wextra -c -o rtw88_vxd.obj rtw88_vxd.c
	$(JWLINK) @rtw88vxd.lnk
	python3 fixvxd.py RTW88.VXD

RTWPROBE.SYS: rtwprobe.c
	$(CC) $(CFLAGS) -shared -Wl,--subsystem,native -Wl,--entry,_DriverEntry@8 \
	  -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--image-base,0x10000 \
	  -nostartfiles -o $@ $< -lntoskrnl -lusbd

RTWTEST.EXE: rtwtest.c
	$(CC) $(CFLAGS) -nostdlib -e _WinMain@16 -o $@ $< -lkernel32 -luser32

AUTOBIND.EXE: autobind.c
	$(CC) $(CFLAGS) -nostdlib -e _WinMain@16 -o $@ $< -lkernel32 -luser32 -ladvapi32

RTWSCAN.EXE: rtwscan.c
	$(CC) $(CFLAGS) -nostdlib -e _mainCRTStartup -Wl,--subsystem,console -o $@ $< -lkernel32 -luser32

clean:
	rm -f RTWPROBE.SYS RTWTEST.EXE AUTOBIND.EXE RTWSCAN.EXE RTW88.VXD RTW88.MAP rtw88vxd.obj rtw88_vxd.obj

.PHONY: all clean
