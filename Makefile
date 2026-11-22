CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -s -march=pentium-mmx -mtune=pentium-mmx -mno-stack-arg-probe

all: RTWPROBE.SYS RTWTEST.EXE AUTOBIND.EXE RTWSCAN.EXE RTW88.SYS

RTWPROBE.SYS: rtwprobe.c
	$(CC) $(CFLAGS) -shared -Wl,--subsystem,native -Wl,--entry,_DriverEntry@8 \
	  -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--image-base,0x10000 \
	  -nostartfiles -o $@ $< -lntoskrnl -lusbd

RTW88.SYS: rtw88.c ndis31.h
	$(CC) $(CFLAGS) -shared -Wl,--subsystem,native -Wl,--entry,_DriverEntry@8 \
	  -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--image-base,0x10000 \
	  -nostartfiles -o $@ $< -lndis -lntoskrnl

RTWTEST.EXE: rtwtest.c
	$(CC) $(CFLAGS) -nostdlib -e _WinMain@16 -o $@ $< -lkernel32 -luser32

AUTOBIND.EXE: autobind.c
	$(CC) $(CFLAGS) -nostdlib -e _WinMain@16 -o $@ $< -lkernel32 -luser32 -ladvapi32

RTWSCAN.EXE: rtwscan.c
	$(CC) $(CFLAGS) -nostdlib -e _mainCRTStartup -Wl,--subsystem,console -o $@ $< -lkernel32 -luser32

clean:
	rm -f RTWPROBE.SYS RTWTEST.EXE AUTOBIND.EXE RTWSCAN.EXE RTW88.SYS

.PHONY: all clean
