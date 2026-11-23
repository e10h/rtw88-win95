/*
 * RTWSCAN.EXE - Milestone 7 RTL8822BU Full Radio Bring-Up & Live Wi-Fi Scanner
 * for Windows 95 OSR 2.1 (Pentium MMX safe, no libc, talks to \\.\RTWPROBE)
 *
 * Ported directly from Linux kernel drivers/net/wireless/realtek/rtw88
 * (mac.c, phy.c, efuse.c, usb.c, rx.c, tx.c, rtw8822b.c, rtw8822b_table.c)
 */
#include <windows.h>
#include "rtw8822b_tables.h"

/* =========================================================================
 * IOCTLs shared with RTWPROBE.SYS (Milestone 7 Generic Hardware Layer)
 * ========================================================================= */
#define IOCTL_RTW_REG_READ      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_REG_WRITE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_REG_BATCH     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x822, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_BULK_RX_RAW   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x823, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_BULK_TX_PIPE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x824, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_GET_PIPES     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x825, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define RTW_OP_WRITE            0
#define RTW_OP_MASK             1
#define RTW_OP_POLL             2
#define RTW_OP_RF_WRITE         3
#define RTW_OP_DELAY_READS      4

typedef struct _RTW_REG_REQ {
    unsigned short Addr;
    unsigned short Width;
    unsigned long  Value;
    long           Status;
} RTW_REG_REQ;

typedef struct _RTW_REG_OP {
    unsigned char  Op;
    unsigned char  Width;
    unsigned short Addr;
    unsigned long  Mask;
    unsigned long  Value;
} RTW_REG_OP;

typedef struct _RTW_REG_BATCH {
    unsigned long Count;
    RTW_REG_OP    Ops[256];
} RTW_REG_BATCH;

typedef struct _RTW_BULK_RX_REQ {
    unsigned long CheckFifo;
    unsigned long MaxLen;
} RTW_BULK_RX_REQ;

typedef struct _RTW_BULK_RX_RAW {
    long          Status;
    unsigned long Length;
    unsigned long RxPktNum;
    unsigned char Data[4096];
} RTW_BULK_RX_RAW;

typedef struct _RTW_BULK_TX_PIPE {
    unsigned long OutPipeIdx;
    unsigned long Length;
    unsigned char Data[4200];
} RTW_BULK_TX_PIPE;

typedef struct _RTW_PIPE_INFO {
    unsigned char  EndpointAddress;
    unsigned char  PipeType;
    unsigned short MaxPacketSize;
} RTW_PIPE_INFO;

typedef struct _RTW_PIPES_RESP {
    unsigned long NumPipes;
    unsigned long NumBulkOut;
    unsigned char BulkInAddr;
    unsigned char BulkOutAddrs[4];
    unsigned char Reserved[3];
    RTW_PIPE_INFO Pipes[8];
} RTW_PIPES_RESP;

/* =========================================================================
 * Static Buffers (no malloc / no libc)
 * ========================================================================= */
#define MAX_NETWORKS 32
#define LOG_BUF_SIZE 32768

typedef struct _WIFI_NET {
    char          Ssid[36];
    unsigned char Bssid[6];
    unsigned char Channel;
    unsigned char Sec;      /* 0=OPEN, 1=WEP, 2=WPA, 3=WPA2 */
    signed char   Dbm;
    unsigned short Beacons;
} WIFI_NET;

static RTW_REG_BATCH    g_Batch;
static RTW_BULK_RX_RAW  g_RxRaw;
static RTW_BULK_TX_PIPE g_TxPipe;
static RTW_PIPES_RESP   g_Pipes;
static unsigned char    g_FwBuf[196608];
static unsigned char    g_LogMap[768];
static WIFI_NET         g_Nets[MAX_NETWORKS];
static int              g_NetCount = 0;
static char             g_LogBuf[LOG_BUF_SIZE];
static unsigned long    g_LogLen = 0;

/* Hardware state */
static HANDLE        g_hDev = INVALID_HANDLE_VALUE;
static unsigned char g_CutVer = 0;
static unsigned char g_RfeOpt = 2;
static unsigned char g_XtalK  = 0x20;
static unsigned char g_Pkg    = 0x0F;
static unsigned char g_Mac[6] = { 0x80, 0x42, 0xEF, 0x68, 0x80, 0x42 };
static unsigned long g_NumBulkOut = 3;
static unsigned long g_BulkOutMaxPkt = 64;
static unsigned short g_RsvdBoundary = 1996;
static unsigned short g_RsvdH2cqAddr = 2036;

/* =========================================================================
 * Minimal No-Libc Helpers (Pentium MMX safe)
 * ========================================================================= */
static void MyZeroMem(void *dst, unsigned long n) {
    volatile unsigned char *p = (volatile unsigned char *)dst;
    while (n--) *p++ = 0;
}

static void MyFillMem(void *dst, unsigned char v, unsigned long n) {
    volatile unsigned char *p = (volatile unsigned char *)dst;
    while (n--) *p++ = v;
}

static void MyMemCpy(void *dst, const void *src, unsigned long n) {
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *s = (const volatile unsigned char *)src;
    while (n--) *d++ = *s++;
}

static int MyMemCmp(const void *a, const void *b, unsigned long n) {
    const unsigned char *p1 = (const unsigned char *)a;
    const unsigned char *p2 = (const unsigned char *)b;
    for (unsigned long i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

static unsigned long MyStrLen(const char *s) {
    const volatile char *p = (const volatile char *)s;
    unsigned long n = 0;
    while (p[n]) n++;
    return n;
}

static void Print(const char *s) {
    unsigned long len = MyStrLen(s);
    HANDLE hStd = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStd && hStd != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(hStd, s, len, &w, NULL);
    }
    for (unsigned long i = 0; i < len && g_LogLen + 1 < LOG_BUF_SIZE; i++) {
        g_LogBuf[g_LogLen++] = s[i];
    }
    g_LogBuf[g_LogLen] = '\0';
}

static void PrintHexN(unsigned long val, int digits) {
    static const char hx[] = "0123456789ABCDEF";
    char buf[16];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < digits; i++) {
        buf[2 + i] = hx[(val >> ((digits - 1 - i) * 4)) & 0xF];
    }
    buf[2 + digits] = '\0';
    Print(buf);
}

static void PrintHex2Raw(unsigned char b) {
    static const char hx[] = "0123456789ABCDEF";
    char buf[3];
    buf[0] = hx[(b >> 4) & 0xF];
    buf[1] = hx[b & 0xF];
    buf[2] = '\0';
    Print(buf);
}

static void PrintDec(unsigned long val) {
    char tmp[16], buf[16];
    int i = 0, j = 0;
    if (val == 0) { Print("0"); return; }
    while (val > 0) { tmp[i++] = (char)('0' + (val % 10)); val /= 10; }
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    Print(buf);
}

static void PrintSigned(long val) {
    if (val < 0) {
        Print("-");
        PrintDec((unsigned long)(-val));
    } else {
        PrintDec((unsigned long)val);
    }
}

static void PrintMac(const unsigned char *m) {
    for (int i = 0; i < 6; i++) {
        PrintHex2Raw(m[i]);
        if (i < 5) Print(":");
    }
}

static void SaveLog(void) {
    HANDLE h = CreateFileA("RTWSCAN.LOG", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, g_LogBuf, g_LogLen, &w, NULL);
        CloseHandle(h);
    }
}

static void WaitForEnter(void) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn && hIn != INVALID_HANDLE_VALUE) {
        char c;
        DWORD r = 0;
        ReadFile(hIn, &c, 1, &r, NULL);
    }
}

/* =========================================================================
 * Register & Batched I/O Primitives
 * ========================================================================= */
static void BatchFlush(void) {
    if (g_Batch.Count == 0) return;
    DWORD br = 0;
    DWORD inLen = (DWORD)(sizeof(unsigned long) + g_Batch.Count * sizeof(RTW_REG_OP));
    DeviceIoControl(g_hDev, IOCTL_RTW_REG_BATCH, &g_Batch, inLen, NULL, 0, &br, NULL);
    g_Batch.Count = 0;
}

static void BatchPush(unsigned char op, unsigned char width, unsigned short addr,
                      unsigned long mask, unsigned long val) {
    if (g_Batch.Count >= 256) BatchFlush();
    RTW_REG_OP *e = &g_Batch.Ops[g_Batch.Count++];
    e->Op    = op;
    e->Width = width;
    e->Addr  = addr;
    e->Mask  = mask;
    e->Value = val;
}

static unsigned long RegReadW(unsigned short addr, unsigned short width) {
    BatchFlush();
    RTW_REG_REQ req;
    req.Addr = addr;
    req.Width = width;
    req.Value = 0;
    req.Status = 0;
    DWORD br = 0;
    DeviceIoControl(g_hDev, IOCTL_RTW_REG_READ, &req, sizeof(req), &req, sizeof(req), &br, NULL);
    return req.Value;
}

static unsigned char  RegRead8(unsigned short addr)  { return (unsigned char)RegReadW(addr, 1); }
static unsigned short RegRead16(unsigned short addr) { return (unsigned short)RegReadW(addr, 2); }
static unsigned long  RegRead32(unsigned short addr) { return RegReadW(addr, 4); }

static void RegWrite8(unsigned short addr, unsigned char v)   { BatchPush(RTW_OP_WRITE, 1, addr, 0xFF, v); }
static void RegWrite16(unsigned short addr, unsigned short v) { BatchPush(RTW_OP_WRITE, 2, addr, 0xFFFF, v); }
static void RegWrite32(unsigned short addr, unsigned long v)  { BatchPush(RTW_OP_WRITE, 4, addr, 0xFFFFFFFFUL, v); }

/* Bitfield shift helper (matches Linux rtw_write32_mask / rtw_write8_mask convention:
 * value is unshifted and shifted by lowest set bit of mask!) */
static unsigned long MaskShift(unsigned long mask) {
    unsigned long s = 0;
    if (!mask) return 0;
    while (!(mask & 1UL)) { mask >>= 1; s++; }
    return s;
}

static void RegMask8(unsigned short addr, unsigned char mask, unsigned char val) {
    unsigned long sh = MaskShift(mask);
    BatchPush(RTW_OP_MASK, 1, addr, mask, ((unsigned long)val << sh) & mask);
}

static void RegMask16(unsigned short addr, unsigned short mask, unsigned short val) {
    unsigned long sh = MaskShift(mask);
    BatchPush(RTW_OP_MASK, 2, addr, mask, ((unsigned long)val << sh) & mask);
}

static void RegMask32(unsigned short addr, unsigned long mask, unsigned long val) {
    unsigned long sh = MaskShift(mask);
    BatchPush(RTW_OP_MASK, 4, addr, mask, (val << sh) & mask);
}

/* Raw mask helper where val is already bit-aligned (like Linux rtw_write*_set/clr) */
static void RegSet8(unsigned short addr, unsigned char bits)   { BatchPush(RTW_OP_MASK, 1, addr, bits, bits); }
static void RegClr8(unsigned short addr, unsigned char bits)   { BatchPush(RTW_OP_MASK, 1, addr, bits, 0); }
static void RegSet16(unsigned short addr, unsigned short bits) { BatchPush(RTW_OP_MASK, 2, addr, bits, bits); }
static void RegSet32(unsigned short addr, unsigned long bits)  { BatchPush(RTW_OP_MASK, 4, addr, bits, bits); }
static void RegClr32(unsigned short addr, unsigned long bits)  { BatchPush(RTW_OP_MASK, 4, addr, bits, 0); }

/* Write both 0xC00 and 0xE00 blocks (Linux rtw_write32s_mask) */
static void RegMask32S(unsigned short addr, unsigned long mask, unsigned long val) {
    RegMask32(addr, mask, val);
    RegMask32((unsigned short)(addr + 0x200), mask, val);
}

static unsigned long RegReadMask32(unsigned short addr, unsigned long mask) {
    unsigned long v = RegRead32(addr);
    return (v & mask) >> MaskShift(mask);
}

static unsigned long RfRead(unsigned char path, unsigned short rfAddr) {
    unsigned short direct = (unsigned short)(((path == 0) ? 0x2800 : 0x2C00) + ((rfAddr & 0xFF) << 2));
    return RegRead32(direct) & 0xFFFFFUL;
}

static void RfWrite(unsigned char path, unsigned short rfAddr, unsigned long mask, unsigned long val) {
    unsigned long sh = MaskShift(mask);
    BatchPush(RTW_OP_RF_WRITE, path, rfAddr, mask & 0xFFFFFUL, (val << sh) & (mask & 0xFFFFFUL));
}

static int PollReg8(unsigned short addr, unsigned char mask, unsigned char target, int maxMs) {
    BatchFlush();
    DWORD start = GetTickCount();
    target &= mask;
    for (;;) {
        if ((RegRead8(addr) & mask) == target) return 1;
        if ((int)(GetTickCount() - start) >= maxMs) return 0;
        Sleep(1);
    }
}

static int PollReg16(unsigned short addr, unsigned short mask, unsigned short target, int maxMs) {
    BatchFlush();
    DWORD start = GetTickCount();
    target &= mask;
    for (;;) {
        if ((RegRead16(addr) & mask) == target) return 1;
        if ((int)(GetTickCount() - start) >= maxMs) return 0;
        Sleep(1);
    }
}

static int PollReg32(unsigned short addr, unsigned long mask, unsigned long target, int maxMs) {
    BatchFlush();
    DWORD start = GetTickCount();
    target &= mask;
    for (;;) {
        if ((RegRead32(addr) & mask) == target) return 1;
        if ((int)(GetTickCount() - start) >= maxMs) return 0;
        Sleep(1);
    }
}

/* =========================================================================
 * Stage 2: EFUSE Read (Physical -> Logical Map) & Sync MAC to RTW88.VXD
 * ========================================================================= */
static unsigned char EfuseReadPhyByte(unsigned short addr, int *ok) {
    unsigned long ctl = RegRead32(0x0030);
    ctl &= ~(0xFFUL | (0x3FFUL << 8));
    ctl |= ((unsigned long)(addr & 0x3FF) << 8);
    RegWrite32(0x0030, ctl & ~0x80000000UL);
    BatchFlush();
    for (int i = 0; i < 200; i++) {
        ctl = RegRead32(0x0030);
        if (ctl & 0x80000000UL) {
            *ok = 1;
            return (unsigned char)(ctl & 0xFF);
        }
    }
    *ok = 0;
    return 0xFF;
}

static int ReadEfuse(void) {
    MyFillMem(g_LogMap, 0xFF, sizeof(g_LogMap));

    /* Switch to Wi-Fi EFUSE bank 0 & disable LDO25 */
    RegMask32(0x0034, 0x00000300UL, 0);
    RegClr8(0x0037, 0x80);
    BatchFlush();

    unsigned short phyIdx = 0;
    unsigned short maxPhy = 1024 - 96; /* phy_efuse_size - ptct_efuse_size */
    int ok = 1;

    while (phyIdx < maxPhy) {
        unsigned char hdr1 = EfuseReadPhyByte(phyIdx, &ok);
        if (!ok || hdr1 == 0xFF) break;

        unsigned char blkIdx, wordEn;
        if ((hdr1 & 0x1F) == 0x0F) {
            unsigned char hdr2 = EfuseReadPhyByte((unsigned short)(phyIdx + 1), &ok);
            if (!ok || hdr2 == 0xFF) break;
            blkIdx = (unsigned char)(((hdr2 & 0xF0) >> 1) | ((hdr1 >> 5) & 0x07));
            wordEn = (unsigned char)(hdr2 & 0x0F);
            phyIdx += 2;
        } else {
            blkIdx = (unsigned char)((hdr1 & 0xF0) >> 4);
            wordEn = (unsigned char)(hdr1 & 0x0F);
            phyIdx += 1;
        }

        for (int i = 0; i < 4; i++) {
            if (wordEn & (1 << i)) continue;
            unsigned short logIdx = (unsigned short)(((unsigned short)blkIdx << 3) + (i << 1));
            if (phyIdx + 2 > maxPhy || logIdx + 2 > sizeof(g_LogMap)) {
                phyIdx = maxPhy;
                break;
            }
            unsigned char b0 = EfuseReadPhyByte(phyIdx++, &ok);
            unsigned char b1 = EfuseReadPhyByte(phyIdx++, &ok);
            if (!ok) { phyIdx = maxPhy; break; }
            g_LogMap[logIdx]     = b0;
            g_LogMap[logIdx + 1] = b1;
        }
    }

    /* Extract RTL8822BU fields (from rtw8822b.h) */
    g_XtalK  = g_LogMap[0xB9];
    g_RfeOpt = g_LogMap[0xCA];
    g_Pkg    = g_LogMap[0x1FB];

    int macValid = 0;
    for (int i = 0; i < 6; i++) {
        if (g_LogMap[0x107 + i] != 0x00 && g_LogMap[0x107 + i] != 0xFF) macValid = 1;
    }
    if (macValid && !(g_LogMap[0x107] & 1)) {
        MyMemCpy(g_Mac, &g_LogMap[0x107], 6);
    }

    if (g_XtalK == 0xFF) g_XtalK = 0x20;
    if (g_Pkg == 0xFF) g_Pkg = 0;
    if (g_RfeOpt != 2 && g_RfeOpt != 3 && g_RfeOpt != 5) {
        Print("  (Note: raw RFE=");
        PrintHexN(g_RfeOpt, 2);
        Print(", defaulting to RFE=2)\r\n");
        g_RfeOpt = 2;
    }

    /* Sync real factory MAC to RTW88.VXD if Dual Mode VxD is loaded */
    HANDLE hVxd = CreateFileA("\\\\.\\RTW88.VXD", 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hVxd != INVALID_HANDLE_VALUE) {
        DWORD br = 0;
        DeviceIoControl(hVxd, 4, g_Mac, 6, NULL, 0, &br, NULL);
        CloseHandle(hVxd);
    }

    return 1;
}

/* =========================================================================
 * Stage 3: RTL8822B Power-On Sequence (mac.c + rtw8822b.c)
 * ========================================================================= */
#define PWR_CMD_WRITE   0
#define PWR_CMD_POLL    1
#define PWR_CMD_DELAY   2
#define PWR_CMD_END     3

#define PWR_CUT_ALL     0xFF
#define PWR_CUT_C       (1 << 3) /* cut_version_to_mask(2) = 1 << 3 */

#define PWR_INTF_USB    0x02
#define PWR_INTF_ALL    0x0F

typedef struct _PWR_CMD {
    unsigned short Offset;
    unsigned char  CutMask;
    unsigned char  IntfMask;
    unsigned char  Cmd;
    unsigned char  Mask;
    unsigned char  Value;
} PWR_CMD;

static const PWR_CMD g_CardDisToEmu[] = {
    {0x004A, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x01, 0x00},
    {0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x98, 0x00},
    {0xFFFF, 0, 0, PWR_CMD_END, 0, 0}
};

static const PWR_CMD g_CardEmuToAct[] = {
    {0x0012, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x02, 0x00},
    {0x0012, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x01, 0x01},
    {0x0020, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x01, 0x01},
    {0x0001, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_DELAY, 1,    1   }, /* 1 ms */
    {0x0000, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x20, 0x00},
    {0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x1C, 0x00},
    {0x0006, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_POLL,  0x02, 0x02},
    {0xFF1A, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0xFF, 0x00},
    {0x0006, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x01, 0x01},
    {0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x80, 0x00},
    {0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x18, 0x00},
    {0x10C3, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x01, 0x01},
    {0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x01, 0x01},
    {0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_POLL,  0x01, 0x00},
    {0x0020, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x08, 0x08},
    {0x10A8, PWR_CUT_C,   PWR_INTF_ALL, PWR_CMD_WRITE, 0xFF, 0x00},
    {0x10A9, PWR_CUT_C,   PWR_INTF_ALL, PWR_CMD_WRITE, 0xFF, 0xEF},
    {0x10AA, PWR_CUT_C,   PWR_INTF_ALL, PWR_CMD_WRITE, 0xFF, 0x0C},
    {0x0029, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0xFF, 0xF9},
    {0x0024, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x04, 0x00},
    {0x00AF, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x20, 0x20},
    {0xFFFF, 0, 0, PWR_CMD_END, 0, 0}
};

static const PWR_CMD g_ActToEmu[] = {
    {0x0093, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x08, 0x00},
    {0x001F, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0xFF, 0x00},
    {0x00EF, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0xFF, 0x00},
    {0xFF1A, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0xFF, 0x30},
    {0x0049, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x02, 0x00},
    {0x0006, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x01, 0x01},
    {0x0002, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x02, 0x00},
    {0x10C3, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x01, 0x00},
    {0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x02, 0x02},
    {0x0005, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_POLL,  0x02, 0x00},
    {0x0020, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x08, 0x00},
    {0x0000, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x20, 0x20},
    {0xFFFF, 0, 0, PWR_CMD_END, 0, 0}
};

static const PWR_CMD g_EmuToDis[] = {
    {0x0007, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0xFF, 0x20},
    {0x0067, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0x20, 0x00},
    {0x004A, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x01, 0x00},
    {0x0081, PWR_CUT_ALL, PWR_INTF_ALL, PWR_CMD_WRITE, 0xC0, 0x00},
    {0x0005, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x18, 0x08},
    {0x0090, PWR_CUT_ALL, PWR_INTF_USB, PWR_CMD_WRITE, 0x02, 0x00},
    {0xFFFF, 0, 0, PWR_CMD_END, 0, 0}
};

static int RunPwrSeq(const PWR_CMD *seq) {
    unsigned char cutMask = (unsigned char)(1U << (g_CutVer + 1));
    for (const PWR_CMD *c = seq; c->Cmd != PWR_CMD_END; c++) {
        if (!(c->IntfMask & PWR_INTF_USB) || !(c->CutMask & cutMask)) continue;
        if (c->Cmd == PWR_CMD_WRITE) {
            BatchPush(RTW_OP_MASK, 1, c->Offset, c->Mask, c->Value & c->Mask);
        } else if (c->Cmd == PWR_CMD_POLL) {
            if (!PollReg8(c->Offset, c->Mask, c->Value, 500)) {
                Print(" [PWR POLL TIMEOUT @ ");
                PrintHexN(c->Offset, 4);
                Print("]");
                return 0;
            }
        } else if (c->Cmd == PWR_CMD_DELAY) {
            BatchFlush();
            Sleep(c->Offset ? c->Offset : 1);
        }
    }
    BatchFlush();
    return 1;
}

static void MacPreSystemCfg(void) {
    RegWrite8(0x001C, 0x00);
    RegSet32(0x0064, (1UL << 29) | (1UL << 28));
    RegClr32(0x004C, (1UL << 26) | (1UL << 25));
    RegSet32(0x0040, (1UL << 2));
    RegClr8(0x0002, 0x03);
    RegClr8(0x001F, 0x07);
    RegClr32(0x00EC, 0x07000000UL);
    BatchFlush();
}

static int MacPowerSwitch(int pwrOn) {
    unsigned char rpwm = RegRead8(0xFE58);
    if (RegRead16(0x0080) == 0xC078) {
        rpwm = (unsigned char)((rpwm ^ 0x80) & 0x80);
        RegWrite8(0xFE58, rpwm);
        BatchFlush();
    }

    int curPwr;
    if (RegRead8(0x0100) == 0xEA) curPwr = 0;
    else if (RegRead8(0x00F5) & 0x01) curPwr = 0;
    else curPwr = 1;

    if (pwrOn == curPwr) return -1; /* EALREADY */

    if (pwrOn) {
        if (!RunPwrSeq(g_CardDisToEmu)) return 0;
        if (!RunPwrSeq(g_CardEmuToAct)) return 0;
        RegClr8(0x00F5, 0x01);
        BatchFlush();
    } else {
        if (!RunPwrSeq(g_ActToEmu)) return 0;
        if (!RunPwrSeq(g_EmuToDis)) return 0;
    }
    return 1;
}

static int RtwMacPowerOn(void) {
    MacPreSystemCfg();
    int r = MacPowerSwitch(1);
    if (r == -1) {
        /* Already powered on: power cycle cleanly */
        MacPowerSwitch(0);
        Sleep(10);
        MacPreSystemCfg();
        r = MacPowerSwitch(1);
    }
    if (r != 1) return 0;

    /* __rtw_mac_init_system_cfg */
    RegSet32(0x1080, (1UL << 16) | (1UL << 8)); /* WL_PLATFORM_RST | DDMA_EN */
    RegSet8(0x0003, 0xDC);                      /* sys_func_en = 0xDC */
    RegMask8(0x1103, 0x0F, 0x0C);               /* REG_CR_EXT + 3 */
    BatchFlush();

    unsigned long tmp = RegRead32(0x0080);
    if (tmp & (1UL << 20)) {
        RegWrite32(0x0080, tmp & ~(1UL << 20));
        RegClr32(0x0040, (1UL << 19));
        BatchFlush();
    }
    return 1;
}

/* =========================================================================
 * Stage 4: Firmware Download (WCPU_3081 Bulk OUT Pipe 0 + DDMA)
 * ========================================================================= */
static int LteCoexRead(unsigned short offset, unsigned long *val) {
    RegWrite32(0x1700, 0x800F0000UL | offset);
    if (!PollReg32(0x1700, (1UL << 29), (1UL << 29), 50)) return 0;
    *val = RegRead32(0x1708);
    return 1;
}

static int LteCoexWrite(unsigned short offset, unsigned long val) {
    RegWrite32(0x1704, val);
    RegWrite32(0x1700, 0xC00F0000UL | offset);
    return PollReg32(0x1700, (1UL << 29), (1UL << 29), 50);
}

static void WlanCpuEnable(int enable) {
    if (enable) {
        RegSet8(0x001D, 0x01); /* BIT_WLMCU_IOIF */
        RegSet8(0x0003, 0x04); /* BIT_FEN_CPUEN */
    } else {
        RegClr8(0x0003, 0x04);
        RegClr8(0x001D, 0x01);
    }
    BatchFlush();
}

static unsigned short CalcTxDescChecksum(const unsigned char *desc) {
    unsigned short sum = 0;
    const unsigned short *w = (const unsigned short *)desc;
    for (int i = 0; i < 16; i++) sum ^= w[i];
    return sum;
}

static int SendFirmwarePkt(unsigned short pgAddr, const unsigned char *data, unsigned long size) {
    unsigned long txSize = size;
    /* Avoid exact multiple of USB Bulk MaxPacketSize (64 on USB 1.1, 512 on USB 2.0) */
    if (((txSize + 48) & (g_BulkOutMaxPkt - 1)) == 0 || ((txSize + 48) & 511) == 0) {
        txSize += 1;
    }

    unsigned char bcnCtrl = RegRead8(0x0550);
    RegWrite16(0x0204, (unsigned short)((pgAddr & 0x0FFF) | 0x8000)); /* BIT_BCN_VALID_V1 (W1C) */

    unsigned char cr1 = RegRead8(0x0101);
    RegWrite8(0x0101, (unsigned char)(cr1 | 0x01)); /* BIT_ENSWBCN >> 8 */
    RegWrite8(0x0550, (unsigned char)((bcnCtrl & ~0x08) | 0x10));
    BatchFlush();

    /* Build 48-byte TX descriptor + payload */
    MyZeroMem(g_TxPipe.Data, 48 + txSize);
    unsigned long *dw = (unsigned long *)g_TxPipe.Data;
    dw[0] = (txSize & 0xFFFFUL) | (48UL << 16) | (1UL << 26); /* TXPKTSIZE | OFFSET=48 | LS=1 */
    dw[1] = (16UL << 8);                                      /* QSEL = TX_DESC_QSEL_BEACON (16) */
    dw[7] = (unsigned long)CalcTxDescChecksum(g_TxPipe.Data);

    MyMemCpy(g_TxPipe.Data + 48, data, size);

    g_TxPipe.OutPipeIdx = 0; /* HIQ -> High Priority -> Bulk OUT Pipe 0 */
    g_TxPipe.Length     = 48 + txSize;

    DWORD br = 0;
    BOOL ok = DeviceIoControl(g_hDev, IOCTL_RTW_BULK_TX_PIPE,
                              &g_TxPipe, 8 + g_TxPipe.Length,
                              NULL, 0, &br, NULL);

    int ready = 0;
    if (ok) {
        ready = PollReg16(0x0204, 0x8000, 0x8000, 500);
    }

    /* Restore */
    RegWrite16(0x0204, (unsigned short)(g_RsvdBoundary | 0x8000));
    RegWrite8(0x0550, bcnCtrl);
    RegWrite8(0x0101, cr1);
    BatchFlush();

    return (ok && ready) ? 1 : 0;
}

static int IddmaDownload(unsigned long src, unsigned long dst, unsigned long len, int firstPart) {
    if (!PollReg32(0x1208, (1UL << 31), 0, 500)) return 0;
    unsigned long ctrl = (1UL << 31) | (1UL << 29) | (len & 0x3FFFFUL);
    if (!firstPart) ctrl |= (1UL << 24); /* BIT_DDMACH0_CHKSUM_CONT */
    RegWrite32(0x1200, src);
    RegWrite32(0x1204, dst);
    RegWrite32(0x1208, ctrl);
    return PollReg32(0x1208, (1UL << 31), 0, 500);
}

static int DownloadFwToMem(const unsigned char *data, unsigned long dst, unsigned long size) {
    unsigned long offset = 0;
    unsigned long rem = size;
    int first = 1;

    RegSet32(0x1208, (1UL << 25)); /* BIT_DDMACH0_RESET_CHKSUM_STS */
    BatchFlush();

    while (rem > 0) {
        unsigned long pkt = (rem >= 4096) ? 4096 : rem;
        if (!SendFirmwarePkt(0, data + offset, pkt)) {
            Print(" [TXPKT FAIL @ ");
            PrintDec(offset);
            Print("]");
            return 0;
        }
        if (!IddmaDownload(0x18780000UL + 48UL, dst + offset, pkt, first)) {
            Print(" [DDMA FAIL @ ");
            PrintDec(offset);
            Print("]");
            return 0;
        }
        first = 0;
        offset += pkt;
        rem -= pkt;
    }

    /* Check DDMA checksum status (0 = OK) */
    unsigned long st = RegRead32(0x1208);
    unsigned char fwCtrl = RegRead8(0x0080);
    if (st & (1UL << 27)) {
        Print(" [DDMA CHKSUM ERR]");
        return 0;
    }
    if (dst < 0x00200000UL) {
        fwCtrl |= (unsigned char)((1 << 3) | (1 << 4)); /* IMEM_DW_OK | IMEM_CHKSUM_OK */
    } else {
        fwCtrl |= (unsigned char)((1 << 5) | (1 << 6)); /* DMEM_DW_OK | DMEM_CHKSUM_OK */
    }
    RegWrite8(0x0080, fwCtrl);
    BatchFlush();
    return 1;
}

static int RtwDownloadFirmware(void) {
    HANDLE hFw = CreateFileA("rtw8822b_fw.bin", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFw == INVALID_HANDLE_VALUE) {
        Print("  [ERR] rtw8822b_fw.bin not found in current folder!\r\n");
        return 0;
    }
    DWORD fwSize = GetFileSize(hFw, NULL);
    if (fwSize < 64 || fwSize > sizeof(g_FwBuf)) {
        CloseHandle(hFw);
        Print("  [ERR] invalid firmware size\r\n");
        return 0;
    }
    DWORD br = 0;
    ReadFile(hFw, g_FwBuf, fwSize, &br, NULL);
    CloseHandle(hFw);
    if (br != fwSize) return 0;

    /* Parse 64-byte rtw_fw_hdr */
    unsigned short sig = *(unsigned short *)(g_FwBuf + 0x00);
    unsigned short ver = *(unsigned short *)(g_FwBuf + 0x04);
    unsigned char  sub = g_FwBuf[0x06];
    unsigned char  memUsage = g_FwBuf[0x18];
    unsigned long  dmemAddr = (*(unsigned long *)(g_FwBuf + 0x20)) & ~0x80000000UL;
    unsigned long  dmemSize = (*(unsigned long *)(g_FwBuf + 0x24)) + 8UL;
    unsigned long  imemSize = (*(unsigned long *)(g_FwBuf + 0x30)) + 8UL;
    unsigned long  ememSize = (memUsage & 0x10) ? ((*(unsigned long *)(g_FwBuf + 0x34)) + 8UL) : 0;
    unsigned long  ememAddr = (*(unsigned long *)(g_FwBuf + 0x38)) & ~0x80000000UL;
    unsigned long  imemAddr = (*(unsigned long *)(g_FwBuf + 0x3C)) & ~0x80000000UL;

    Print("  FW sig="); PrintHexN(sig, 4);
    Print(" v"); PrintDec(ver); Print("."); PrintDec(sub);
    Print(" DMEM="); PrintDec(dmemSize);
    Print(" IMEM="); PrintDec(imemSize);
    Print(" total="); PrintDec(fwSize); Print("\r\n");

    if (64UL + dmemSize + imemSize + ememSize != fwSize) {
        Print("  [ERR] FW header/file size mismatch\r\n");
        return 0;
    }

    unsigned long lteBckp = 0;
    int hasLte = LteCoexRead(0x38, &lteBckp);

    WlanCpuEnable(0);

    /* Backup 6 registers (download_firmware_reg_backup) */
    unsigned char  bk_pq1 = RegRead8(0x010D);
    RegWrite8(0x010D, (unsigned char)(3 << 6)); /* HIQ -> High Priority */

    unsigned char  bk_cr = RegRead8(0x0100);
    RegWrite8(0x0100, 0x05); /* HCI_TXDMA_EN | TXDMA_EN */
    RegWrite32(0x1330, 0x80000000UL); /* REG_H2CQ_CSR = BIT_H2CQ_FULL */

    unsigned short bk_fp1 = RegRead16(0x0230);
    unsigned long  bk_rq2 = RegRead32(0x022C) | 0x80000000UL;
    RegWrite16(0x0230, 0x0200);
    RegWrite32(0x022C, bk_rq2);

    unsigned char  bk_bcn = RegRead8(0x0550);
    RegWrite8(0x0550, (unsigned char)((bk_bcn & ~0x08) | 0x10));
    BatchFlush();

    /* Reset platform */
    RegClr8(0x1082, 0x01);
    RegClr8(0x0009, 0x40);
    RegSet8(0x1082, 0x01);
    RegSet8(0x0009, 0x40);
    BatchFlush();

    /* Enable MCUFWDL */
    unsigned short fwVal = (unsigned short)((RegRead16(0x0080) & 0x3800) | 0x0001);
    RegWrite16(0x0080, fwVal);
    BatchFlush();

    Print("  Downloading DMEM... ");
    int ok = DownloadFwToMem(g_FwBuf + 64, dmemAddr, dmemSize);
    Print(ok ? "OK\r\n" : "FAIL\r\n");

    if (ok) {
        Print("  Downloading IMEM... ");
        ok = DownloadFwToMem(g_FwBuf + 64 + dmemSize, imemAddr, imemSize);
        Print(ok ? "OK\r\n" : "FAIL\r\n");
    }

    if (ok && ememSize) {
        Print("  Downloading EMEM... ");
        ok = DownloadFwToMem(g_FwBuf + 64 + dmemSize + imemSize, ememAddr, ememSize);
        Print(ok ? "OK\r\n" : "FAIL\r\n");
    }

    /* Restore 6 registers */
    RegWrite8(0x010D, bk_pq1);
    RegWrite8(0x0100, bk_cr);
    RegWrite32(0x1330, 0x80000000UL);
    RegWrite16(0x0230, bk_fp1);
    RegWrite32(0x022C, bk_rq2);
    RegWrite8(0x0550, bk_bcn);
    BatchFlush();

    if (!ok) {
        RegClr8(0x0080, 0x01);
        RegSet8(0x0003, 0x04);
        BatchFlush();
        return 0;
    }

    /* download_firmware_end_flow */
    RegWrite32(0x0210, 0x04);
    BatchFlush();
    unsigned short fwCtrl = RegRead16(0x0080);
    if ((fwCtrl & 0x0050) != 0x0050) {
        Print("  [ERR] Checksum bits not set: "); PrintHexN(fwCtrl, 4); Print("\r\n");
        return 0;
    }
    fwCtrl = (unsigned short)((fwCtrl | 0x4000) & ~0x0001);
    RegWrite16(0x0080, fwCtrl);
    BatchFlush();

    /* Boot 32-bit RISC CPU */
    WlanCpuEnable(1);
    if (hasLte) LteCoexWrite(0x38, lteBckp);

    /* Poll FW_READY (0xC078 masked by ~0x3000) */
    int ready = PollReg16(0x0080, 0xCFFF, 0xC078, 1500);
    unsigned short finalCtrl = RegRead16(0x0080);
    Print("  CPU Boot REG_MCUFW_CTRL="); PrintHexN(finalCtrl, 4);
    Print(ready ? " [FW_READY!]\r\n" : " [TIMEOUT]\r\n");
    return ready;
}

/* =========================================================================
 * Stage 5: MAC, LLT, H2C & USB 1.1 Burst/RX Init (mac.c + rtw8822b.c + usb.c)
 * ========================================================================= */
static int RtwMacInit(void) {
    /* rqpn_table_8822b indexed by bulkout_num (2, 3, or 4)
     * Mapping values: EXTRA=0, LOW=1, NORMAL=2, HIGH=3 */
    unsigned char vo, vi, be, bk, mg, hi;
    unsigned short hq, lq, nq, exq, gapq;

    if (g_NumBulkOut == 2) {
        vo = 2; vi = 2; be = 2; bk = 3; mg = 3; hi = 3;
        hq = 64; lq = 64; nq = 0; exq = 0; gapq = 1;
    } else if (g_NumBulkOut == 3) {
        vo = 2; vi = 2; be = 1; bk = 1; mg = 3; hi = 3;
        hq = 64; lq = 64; nq = 64; exq = 0; gapq = 1;
    } else {
        vo = 2; vi = 2; be = 1; bk = 1; mg = 0; hi = 3;
        hq = 64; lq = 64; nq = 64; exq = 64; gapq = 1;
    }

    /* 1. txdma_queue_mapping */
    unsigned short pqMap = (unsigned short)(
        ((unsigned short)vo << 4)  |
        ((unsigned short)vi << 6)  |
        ((unsigned short)be << 8)  |
        ((unsigned short)bk << 10) |
        ((unsigned short)mg << 12) |
        ((unsigned short)hi << 14));
    RegWrite16(0x010C, pqMap);
    RegWrite8(0x0100, 0x00);
    RegWrite8(0x0100, 0xFF);        /* MAC_TRX_ENABLE */
    RegWrite32(0x1330, 0x80000000UL); /* REG_H2CQ_CSR = BIT_H2CQ_FULL */
    RegSet8(0x010C, 0x01);          /* BIT_RXDMA_ARBBW_EN */

    /* 2. priority_queue_cfg (txff=2048 pgs, rsvd=8+24+8+8+0+4=52 -> acq=1996) */
    g_RsvdBoundary = 2048 - 52; /* 1996 */
    g_RsvdH2cqAddr = 2048 - 4 - 8; /* 2036 */
    unsigned short pubq = (unsigned short)(g_RsvdBoundary - hq - lq - nq - exq - gapq);

    RegWrite16(0x0230, hq);
    RegWrite16(0x0234, lq);
    RegWrite16(0x0238, nq);
    RegWrite16(0x023C, exq);
    RegWrite16(0x0240, pubq);
    RegSet32(0x022C, 0x80000000UL); /* BIT_LD_RQPN */

    RegWrite16(0x0204, g_RsvdBoundary);
    RegSet8(0x0422, 0x10); /* BIT_EN_WR_FREE_TAIL >> 16 */

    RegWrite16(0x0424, g_RsvdBoundary);
    RegWrite16(0x0206, g_RsvdBoundary);
    RegWrite16(0x0456, g_RsvdBoundary);
    RegWrite32(0x011C, 24576UL - 256UL - 1UL); /* REG_RXFF_BNDY = 24319 */

    /* USB TX agg desc num = 3 */
    RegMask8(0x0208, 0xF0, 3);
    RegWrite8(0x020B, 3);
    RegSet8(0x020D, 0x02);

    /* Auto Init LLT */
    RegSet8(0x0208, 0x01);
    if (!PollReg8(0x0208, 0x01, 0x00, 500)) {
        Print("  [ERR] AUTO_INIT_LLT_V1 timeout\r\n");
        return 0;
    }
    RegWrite8(0x0103, 0x00);

    /* 3. init_h2c */
    unsigned long h2cAddr = (unsigned long)g_RsvdH2cqAddr << 7;
    unsigned long h2cSize = 8UL << 7;
    RegMask32(0x0244, 0x0003FFFFUL, h2cAddr);
    RegMask32(0x024C, 0x0003FFFFUL, h2cAddr);
    RegMask32(0x0248, 0x0003FFFFUL, h2cAddr + h2cSize);
    RegMask8(0x0254, 0x03, 0x01);
    RegMask8(0x0254, 0x04, 0x01);
    RegSet8(0x020D, 0x80);

    /* 4. rtw8822b_mac_init */
    RegClr8(0x04BC, 0x40);          /* BIT_PRE_TX_CMD */
    RegWrite8(0x0455, 0x70);        /* AMPDU_MAX_TIME */
    RegSet8(0x045E, 0x04);          /* BIT_EN_EOF_V1 */
    RegWrite32(0x04C8, 0x202008FFUL); /* REG_PROT_MODE_CTRL */
    RegWrite16(0x04CE, 0x0801);     /* REG_BAR_MODE_CTRL + 2 */
    RegWrite8(0x1448, 0x06);
    RegWrite8(0x144A, 0x06);
    RegWrite8(0x144C, 0x06);
    RegWrite8(0x144E, 0x06);

    /* EDCA & Beacon control */
    RegClr8(0x05B4, 0x70);
    RegWrite16(0x0522, 0x0000);     /* REG_TXPAUSE = 0 */
    RegWrite8(0x051B, 0x09);        /* SLOT */
    RegWrite8(0x0512, 0x19);        /* PIFS */
    RegWrite32(0x0514, 0x10100E0AUL); /* SIFS */
    RegWrite16(0x0502, 0x0186);
    RegWrite16(0x0506, 0x03BC);
    RegWrite32(0x0544, 0x001B0005UL);
    RegWrite16(0x055E, 0x3030);
    RegSet8(0x0550, 0x08);          /* BIT_EN_BCN_FUNCTION */
    RegWrite32(0x0540, 0x00006404UL);
    RegWrite8(0x0558, 0x04);
    RegWrite8(0x0559, 0x02);
    RegClr8(0x0521, 0x10);

    /* WMAC RX filters & RCR (accept all mgmt, ctrl, data, promiscuous) */
    RegWrite16(0x06A0, 0xFFFF);     /* REG_RXFLTMAP0 (mgmt) */
    RegWrite16(0x06A2, 0x0FFF);     /* REG_RXFLTMAP1 (ctrl) */
    RegWrite16(0x06A4, 0xFFFF);     /* REG_RXFLTMAP2 (data) */
    RegWrite32(0x0608, 0xE400220FUL); /* WLAN_RCR_CFG | BIT_AAP */
    RegWrite8(0x060C, 24);          /* 12288 >> 9 */
    RegWrite8(0x0606, 0x30);
    RegWrite8(0x0605, 0x30);
    RegWrite32(0x07D8, 0xB0810041UL);
    RegWrite8(0x07D4, 0x98);
    RegSet8(0x0718, 0x40);          /* REG_SND_PTCL_CTRL: BIT_DIS_CHK_VHTSIGB_CRC */

    /* 5. rtw_drv_info_cfg (32-byte PHY status + rxdesc len=0 fix) */
    RegWrite8(0x060F, 4);           /* PHY_STATUS_SIZE = 4 (32 bytes) */
    RegMask8(0x0115, 0x0F, 0x0F);   /* REG_TRXFF_BNDY + 1 */
    RegSet32(0x0608, (1UL << 28));  /* BIT_APP_PHYSTS */
    RegClr32(0x07D4, (1UL << 8) | (1UL << 9));

    /* 6. USB burst size (64B for USB 1.1 OHCI, 512B for USB 2.0) + disable RX agg */
    unsigned char burstSz = (g_BulkOutMaxPkt <= 64) ? 2 : 1;
    unsigned char rxdma = (unsigned char)(0x0C | 0x02 | (burstSz << 4));
    RegWrite8(0x0290, rxdma);
    RegSet16(0x020C, (1U << 9));    /* BIT_DROP_DATA_EN */

    /* rtw_usb_dynamic_rx_agg_v1(false): immediate RX delivery */
    RegSet8(0x010C, 0x04);          /* BIT_RXDMA_AGG_EN */
    RegClr8(0x0283, 0x80);
    RegWrite16(0x0280, 0x0100);     /* size=0, timeout=1 */

    BatchFlush();
    return 1;
}

/* =========================================================================
 * Stage 6: PHY Tables, BB/RF Power-On, TRX Mode & Channel Tuning (rtw8822b.c)
 * ========================================================================= */
#define TBL_MAC  0
#define TBL_BB   1
#define TBL_AGC  2
#define TBL_RF_A 3
#define TBL_RF_B 4

static int CheckPhyCond(unsigned long c) {
    unsigned char drvCut = g_CutVer ? g_CutVer : 15;
    unsigned char drvPkg = g_Pkg ? g_Pkg : 15;
    unsigned char drvRfe = g_RfeOpt;

    unsigned char cut  = (unsigned char)((c >> 24) & 0x0F);
    unsigned char pkg  = (unsigned char)((c >> 12) & 0x0F);
    unsigned char intf = (unsigned char)((c >> 8)  & 0x0F);
    unsigned char rfe  = (unsigned char)(c & 0xFF);

    if (cut && cut != drvCut) return 0;
    if (pkg && pkg != drvPkg) return 0;
    if (intf && intf != 2) return 0; /* INTF_USB = BIT(1) = 2 */
    if (rfe != drvRfe) return 0;
    return 1;
}

static unsigned long LoadPhyTable(const unsigned long *arr, unsigned long len, int kind) {
    int isMatched = 1, isSkipped = 0;
    unsigned long posCond = 0;
    unsigned long writes = 0;

    for (unsigned long i = 0; i + 1 < len; i += 2) {
        unsigned long w0 = arr[i];
        unsigned long w1 = arr[i + 1];
        unsigned long pos = (w0 >> 31) & 1UL;
        unsigned long neg = (w0 >> 30) & 1UL;
        unsigned long br  = (w0 >> 28) & 3UL;

        if (pos) {
            if (br == 3) {          /* BRANCH_ENDIF */
                isMatched = 1;
                isSkipped = 0;
            } else if (br == 2) {   /* BRANCH_ELSE */
                isMatched = isSkipped ? 0 : 1;
            } else {                /* BRANCH_IF / ELIF */
                posCond = w0;
            }
        } else if (neg) {
            if (!isSkipped) {
                if (CheckPhyCond(posCond)) {
                    isMatched = 1;
                    isSkipped = 1;
                } else {
                    isMatched = 0;
                    isSkipped = 0;
                }
            } else {
                isMatched = 0;
            }
        } else if (isMatched) {
            unsigned short addr = (unsigned short)(w0 & 0xFFFF);
            if (kind == TBL_MAC) {
                RegWrite8(addr, (unsigned char)(w1 & 0xFF));
                writes++;
            } else if (kind == TBL_BB) {
                if (addr == 0xFE)      { BatchFlush(); Sleep(50); }
                else if (addr == 0xFD) { BatchFlush(); Sleep(5);  }
                else if (addr == 0xFC) { BatchFlush(); Sleep(1);  }
                else if (addr == 0xFB || addr == 0xFA || addr == 0xF9) {
                    BatchPush(RTW_OP_DELAY_READS, 1, 0, 0, 2);
                } else {
                    RegWrite32(addr, w1);
                    writes++;
                }
            } else if (kind == TBL_AGC) {
                RegWrite32(addr, w1);
                writes++;
            } else if (kind == TBL_RF_A || kind == TBL_RF_B) {
                if (addr == 0xFFE) { BatchFlush(); Sleep(50); }
                else if (addr == 0xFE) { BatchFlush(); Sleep(1); }
                else {
                    RfWrite((kind == TBL_RF_A) ? 0 : 1, addr, 0xFFFFFUL, w1);
                    writes++;
                }
            }
        }
    }
    BatchFlush();
    return writes;
}

static void SetChannelRfe(unsigned char ch) {
    /* rtw8822b_rfe_info: [2]=efem, [3]=ifem, [5]=ifem */
    if (g_RfeOpt == 2) {
        if (ch <= 14) {
            RegMask32S(0x0CB0, 0x00FFFFFFUL, 0x705770UL);
            RegMask32S(0x0CB4, 0x0000FF00UL, 0x57UL);
            RegMask32S(0x0CB8, (1UL << 4), 0);
        } else {
            RegMask32S(0x0CB0, 0x00FFFFFFUL, 0x177517UL);
            RegMask32S(0x0CB4, 0x0000FF00UL, 0x75UL);
            RegMask32S(0x0CB8, (1UL << 5), 0);
        }
        RegMask32S(0x0CBC, (1UL << 11) | (1UL << 10) | 0x3FUL, 0);
        RegMask32S(0x0CA0, 0x0000FFFFUL, 0xA501UL); /* 2TX/2RX (BB_PATH_AB) */
    } else {
        if (ch <= 14) {
            RegMask32S(0x0CB0, 0x00FFFFFFUL, 0x745774UL);
            RegMask32S(0x0CB4, 0x0000FF00UL, 0x57UL);
        } else {
            RegMask32S(0x0CB0, 0x00FFFFFFUL, 0x477547UL);
            RegMask32S(0x0CB4, 0x0000FF00UL, 0x75UL);
        }
        RegMask32S(0x0CBC, (1UL << 11) | (1UL << 10) | 0x3FUL, 0);
        RegMask32S(0x0CA0, 0x0000FFFFUL, (ch <= 14) ? 0xA501UL : 0xA5A5UL);
    }
}

static void SetChannelCca(unsigned char ch) {
    unsigned long r82c, r830, r838;
    if (g_RfeOpt == 2) {
        /* 2R_2G = cca_ifem_ccut[1], 2R_5G = cca_efem_ccut[3] */
        if (ch <= 14) {
            r82c = 0x75C97010UL; r830 = 0x79A0EAACUL; r838 = 0x87746341UL;
        } else {
            r82c = 0x75B76010UL; r830 = 0x79A0EAAAUL; r838 = 0x87766431UL;
        }
    } else {
        /* cca_ifem_ccut_ext */
        if (ch <= 14) {
            r82c = 0x75DA8010UL; r830 = 0x97A0EAACUL; r838 = 0x86666341UL;
        } else {
            r82c = 0x75DA8010UL; r830 = (g_RfeOpt == 5) ? 0x79A0EA28UL : 0x79A0EAAAUL; r838 = 0x86666361UL;
        }
    }
    RegWrite32(0x082C, r82c);
    RegWrite32(0x0830, r830);
    RegWrite32(0x0838, r838);
    if (g_RfeOpt == 2 && ch > 14 && g_CutVer != 1) {
        RegWrite32(0x083C, 0x9194B2B9UL);
    }
}

static void ToggleIgi(void) {
    unsigned long igi = RegReadMask32(0x0C50, 0x7FUL);
    if (igi < 2) igi = 0x1C;
    RegMask32(0x0C50, 0x7FUL, igi - 2);
    RegMask32(0x0C50, 0x7FUL, igi);
    RegMask32(0x0E50, 0x7FUL, igi - 2);
    RegMask32(0x0E50, 0x7FUL, igi);
    RegMask32(0x0808, 0xFFUL, 0x00);
    RegMask32(0x0808, 0xFFUL, 0x33); /* BB_PATH_AB | (BB_PATH_AB << 4) */
    BatchFlush();
}

static void RtwConfigTrxMode(void) {
    /* BB_PATH_AB = 3 (2T2R) */
    RegMask32(0x0C08, 0xFFFFUL, 0x3231);
    RegMask32(0x0E08, 0xFFFFUL, 0x3231);
    RegMask32(0x093C, (1UL << 19) | (1UL << 18), 0x3);
    RegMask32(0x080C, (1UL << 29) | (1UL << 28), 0x1);
    RegMask32(0x080C, (1UL << 30), 0x1);
    RegMask32(0x093C, 0xFFF00000UL, 0x001);
    RegMask32(0x0A04, 0xF0000000UL, 0x8);
    RegMask32(0x0940, 0xFFF0UL, 0x43);
    RegMask32(0x080C, 0xFFUL, 0x33);
    RegMask32(0x0A2C, (1UL << 22), 0x0);
    RegMask32(0x0A2C, (1UL << 18), 0x0);
    RegMask32(0x0A04, 0x0F000000UL, 0x0);
    RegMask32(0x0808, 0xFFUL, 0x33);
    RegMask32(0x1904, (1UL << 16), 0x1);
    RegMask32(0x0800, (1UL << 28), 0x1);
    RegMask32(0x0850, (1UL << 23), 0x1);
    BatchFlush();

    /* RF Mode Table setup on Path A (rtw8822b.c lines 814-836) */
    for (int cnt = 0; cnt < 50; cnt++) {
        RfWrite(0, 0xEF, 0xFFFFFUL, 0x80000UL);
        RfWrite(0, 0x33, 0xFFFFFUL, 0x00001UL);
        BatchFlush();
        if (RfRead(0, 0x33) == 0x00001UL) break;
    }
    RfWrite(0, 0xEF, 0xFFFFFUL, 0x80000UL);
    RfWrite(0, 0x33, 0xFFFFFUL, 0x00001UL);
    RfWrite(0, 0x3E, 0xFFFFFUL, 0x00034UL);
    RfWrite(0, 0x3F, 0xFFFFFUL, 0x4080CUL);
    RfWrite(0, 0xEF, 0xFFFFFUL, 0x00000UL);
    RfWrite(0, 0xEF, 0xFFFFFUL, 0x00000UL);
    BatchFlush();

    ToggleIgi();
    SetChannelCca(1);
    SetChannelRfe(1);
    BatchFlush();
}

static void RtwPhySetParam(void) {
    /* Power on BB & RF domains */
    RegSet8(0x0002, 0x03);         /* BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST */
    RegSet8(0x001F, 0x07);         /* BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB */
    RegSet32(0x00EC, 0x07000000UL);/* BIT_WLRF1_BBRF_EN */
    RegClr32(0x0808, 0x30000000UL);/* BIT_RX_PSEL_RST */
    BatchFlush();

    /* Load MAC, BB, AGC, RF_A, RF_B tables */
    unsigned long nMac = LoadPhyTable(rtw8822b_mac,  RTW8822B_MAC_LEN,  TBL_MAC);
    unsigned long nBb  = LoadPhyTable(rtw8822b_bb,   RTW8822B_BB_LEN,   TBL_BB);
    unsigned long nAgc = LoadPhyTable(rtw8822b_agc,  RTW8822B_AGC_LEN,  TBL_AGC);
    unsigned long nRfA = LoadPhyTable(rtw8822b_rf_a, RTW8822B_RF_A_LEN, TBL_RF_A);
    unsigned long nRfB = LoadPhyTable(rtw8822b_rf_b, RTW8822B_RF_B_LEN, TBL_RF_B);

    Print("  Tables loaded: MAC="); PrintDec(nMac);
    Print(" BB="); PrintDec(nBb);
    Print(" AGC="); PrintDec(nAgc);
    Print(" RFA="); PrintDec(nRfA);
    Print(" RFB="); PrintDec(nRfB); Print("\r\n");

    /* Crystal cap */
    unsigned long xcap = g_XtalK & 0x3FUL;
    RegMask32(0x0024, 0x7E000000UL, xcap);
    RegMask32(0x0028, 0x0000007EUL, xcap);

    /* Post-init: release RX path select reset */
    RegSet32(0x0808, 0x30000000UL);
    BatchFlush();

    /* TRX Mode (2T2R) */
    RtwConfigTrxMode();

    /* rtw8822b_phy_rfe_init */
    RegMask32(0x0064, (1UL << 29) | (1UL << 28), 0x3);
    RegMask32(0x004C, (1UL << 26) | (1UL << 25), 0x0);
    RegMask32(0x0040, (1UL << 2), 0x1);
    RegMask32(0x1990, 0x3FUL, 0x30);
    RegMask32(0x1990, (1UL << 11) | (1UL << 10), 0x3);
    RegMask32(0x0974, 0x3FUL, 0x3F);
    RegMask32(0x0974, (1UL << 11) | (1UL << 10), 0x3);

    /* Route RF DPDT antenna switch firmly to Wi-Fi (not Bluetooth) */
    RegMask8(0x004E, 0x80, 0x0); /* 0x4c[23] = 0 */
    RegMask8(0x004F, 0x01, 0x1); /* 0x4c[24] = 1 (DPDT_WL_SEL) */

    /* Adaptivity & BF init */
    RegMask32(0x08A4, 0x000000FFUL, 0x7F);
    RegMask32(0x08A4, 0x0000FF00UL, 0x7F);
    RegClr32(0x0520, (1UL << 15));
    RegSet32(0x0524, (1UL << 11));
    RegMask32(0x0944, (1UL << 29) | (1UL << 28), 1);
    RegMask32(0x08A0, 0x03UL, 0);
    RegSet32(0x08DC, (1UL << 5));
    RegWrite32(0x1C94, 0xAFFFAFFFUL);

    /* Program factory MAC into REG_MACID (0x0610) & ensure promiscuous RCR */
    for (int i = 0; i < 6; i++) RegWrite8((unsigned short)(0x0610 + i), g_Mac[i]);
    RegWrite16(0x06A0, 0xFFFF);
    RegWrite16(0x06A2, 0x0FFF);
    RegWrite16(0x06A4, 0xFFFF);
    RegWrite32(0x0608, 0xF400220FUL); /* APP_FCS|MIC|ICV|PHYSTS + AAP|APM|AM|AB|AMF|ADF|HTC */

    /* DIG max coverage for scanning (0x1C) */
    RegMask32(0x0C50, 0x7FUL, 0x1C);
    RegMask32(0x0E50, 0x7FUL, 0x1C);
    BatchFlush();
}

static void RtwSetChannel2G(unsigned char ch) {
    /* 1. set_channel_bb (2.4 GHz, BW20) */
    RegMask32(0x0808, (1UL << 28), 0x1);
    RegMask32(0x0454, (1UL << 7),  0x0);
    RegMask32(0x0A80, (1UL << 18), 0x0);
    RegMask32(0x0814, 0x0000FC00UL, 15);
    RegMask32(0x0958, 0x1FUL, 0x0);
    RegMask32(0x0860, 0x1FFE0000UL, 0x96A);
    if (ch == 14) {
        RegWrite32(0x0A24, 0x00006577UL);
        RegMask32(0x0A28, 0xFFFFUL, 0x0000);
    } else {
        RegWrite32(0x0A24, 0x384F6577UL);
        RegMask32(0x0A28, 0xFFFFUL, 0x1525);
    }
    RegMask32(0x0CBC, 0x0300UL, 0x2);

    /* BW20 ADC clock */
    BatchFlush();
    unsigned long adc = RegRead32(0x08AC);
    adc &= 0xFFCFFC00UL;
    RegWrite32(0x08AC, adc);
    RegMask32(0x08C4, (1UL << 30), 0x1);

    /* 2. rtw_set_channel_mac (BW20, 2.4G) */
    RegWrite8(0x0483, 0x00);
    RegClr32(0x0668, (1UL << 7) | (1UL << 8));
    RegMask32(0x0024, (1UL << 20) | (1UL << 21), 0); /* 80M MAC CLK */
    RegWrite8(0x055C, 80);
    RegWrite8(0x0638, 80);
    RegClr8(0x0454, (1 << 7));
    BatchFlush();

    /* 3. rtw8822b_set_channel_rf (2.4G, BW20) */
    unsigned long rf18 = RfRead(0, 0x18);
    rf18 &= ~((1UL << 16) | (1UL << 9) | (1UL << 8) | 0xFFUL | (1UL << 18) | (1UL << 17) | (1UL << 11) | (1UL << 10));
    rf18 |= (unsigned long)ch | (1UL << 11) | (1UL << 10); /* RF18_BAND_2G=0 | ch | RF18_BW_20M */

    RfWrite(0, 0xBE, (1UL << 17) | (1UL << 16) | (1UL << 15), 0);
    RfWrite(0, 0xDF, (1UL << 18), 0);
    RfWrite(0, 0x18, 0xFFFFFUL, rf18);
    RfWrite(1, 0x18, 0xFFFFFUL, rf18);
    RfWrite(0, 0xB8, (1UL << 19), 0);
    RfWrite(0, 0xB8, (1UL << 19), 1);

    /* 4. set_channel_rxdfir (BW20) */
    RegMask32(0x0948, (1UL << 29) | (1UL << 28), 0x2);
    RegMask32(0x094C, (1UL << 29) | (1UL << 28), 0x2);
    RegMask32S(0x0C20, (1UL << 31), 0x1);
    BatchFlush();

    /* 5. toggle_igi + CCA + RFE + max coverage DIG */
    ToggleIgi();
    SetChannelCca(ch);
    SetChannelRfe(ch);
    RegMask32(0x0C50, 0x7FUL, 0x1C);
    RegMask32(0x0E50, 0x7FUL, 0x1C);
    BatchFlush();
}

/* =========================================================================
 * Stage 7: 802.11 RX Descriptor & Beacon / Probe Response Parser
 * ========================================================================= */
static void RecordNetwork(const char *ssid, const unsigned char *bssid,
                          unsigned char ch, unsigned char sec, signed char dbm) {
    for (int i = 0; i < g_NetCount; i++) {
        if (MyMemCmp(g_Nets[i].Bssid, bssid, 6) == 0) {
            g_Nets[i].Beacons++;
            if (dbm > g_Nets[i].Dbm) g_Nets[i].Dbm = dbm;
            if (g_Nets[i].Ssid[0] == '<' && ssid[0] != '<') {
                unsigned long l = MyStrLen(ssid);
                if (l > 32) l = 32;
                MyMemCpy(g_Nets[i].Ssid, ssid, l);
                g_Nets[i].Ssid[l] = '\0';
            }
            return;
        }
    }
    if (g_NetCount < MAX_NETWORKS) {
        int idx = g_NetCount++;
        unsigned long l = MyStrLen(ssid);
        if (l > 32) l = 32;
        MyMemCpy(g_Nets[idx].Ssid, ssid, l);
        g_Nets[idx].Ssid[l] = '\0';
        MyMemCpy(g_Nets[idx].Bssid, bssid, 6);
        g_Nets[idx].Channel = ch;
        g_Nets[idx].Sec     = sec;
        g_Nets[idx].Dbm     = dbm;
        g_Nets[idx].Beacons = 1;

        static const char *secStr[] = { "OPEN", "WEP ", "WPA ", "WPA2" };
        Print("    + CH"); PrintDec(ch);
        if (ch < 10) Print(" ");
        Print(" ["); Print(secStr[sec & 3]); Print("] ");
        PrintSigned(dbm); Print("dBm ");
        PrintMac(bssid); Print("  \""); Print(g_Nets[idx].Ssid); Print("\"\r\n");
    }
}

static void ParseBeaconOrProbeResp(const unsigned char *frame, unsigned long len,
                                   unsigned char curCh, signed char dbm) {
    if (len < 36) return;
    const unsigned char *bssid = frame + 16;
    unsigned short cap = (unsigned short)(frame[34] | ((unsigned short)frame[35] << 8));
    unsigned char sec = (cap & 0x0010) ? 1 : 0; /* Privacy bit -> WEP unless WPA/RSN IE found */
    unsigned char dsCh = curCh;

    char ssid[36];
    ssid[0] = '\0';

    unsigned long pos = 36;
    while (pos + 2 <= len) {
        unsigned char id = frame[pos];
        unsigned char elen = frame[pos + 1];
        if (pos + 2 + elen > len) break;
        const unsigned char *ed = frame + pos + 2;

        if (id == 0) {
            unsigned long cl = elen;
            if (cl > 32) cl = 32;
            int printable = 0;
            for (unsigned long k = 0; k < cl; k++) {
                char c = (char)ed[k];
                ssid[k] = (c >= 32 && c <= 126) ? c : '.';
                if (c != 0) printable = 1;
            }
            ssid[cl] = '\0';
            if (!printable) ssid[0] = '\0';
        } else if (id == 3 && elen == 1 && ed[0] >= 1 && ed[0] <= 14) {
            dsCh = ed[0];
        } else if (id == 48) {
            sec = 3; /* RSN = WPA2 */
        } else if (id == 221 && elen >= 4 &&
                   ed[0] == 0x00 && ed[1] == 0x50 && ed[2] == 0xF2 && ed[3] == 0x01) {
            if (sec < 2) sec = 2; /* WPA */
        }
        pos += 2 + elen;
    }

    if (ssid[0] == '\0') {
        MyMemCpy(ssid, "<Hidden>", 9);
    }
    RecordNetwork(ssid, bssid, dsCh, sec, dbm);
}

static void ProcessRxBuffer(const unsigned char *buf, unsigned long len, unsigned char ch,
                            unsigned long *chFrames, unsigned long *totalFrames,
                            unsigned long *totalBeacons, unsigned long *totalC2h,
                            unsigned long *totalCrcErr) {
    unsigned long off = 0;
    while (off + 24 <= len) {
        const unsigned char *p = buf + off;
        unsigned long w0 = *(const unsigned long *)(p + 0);
        unsigned long w2 = *(const unsigned long *)(p + 8);

        unsigned long pktLen  = w0 & 0x3FFFUL;
        unsigned long crcErr  = (w0 >> 14) & 1UL;
        unsigned long drvSz   = ((w0 >> 16) & 0xFUL) * 8UL;
        unsigned long shift   = (w0 >> 24) & 3UL;
        unsigned long phySt   = (w0 >> 26) & 1UL;
        unsigned long isC2h   = (w2 >> 28) & 1UL;

        unsigned long pktOff  = 24UL + shift + drvSz;
        if (pktLen == 0 || pktOff + pktLen > len - off) break;

        if (isC2h) {
            (*totalC2h)++;
        } else {
            (*totalFrames)++;
            (*chFrames)++;
            if (crcErr) {
                (*totalCrcErr)++;
            } else {
                signed char dbm = -80;
                if (phySt && drvSz >= 8) {
                    unsigned char pwdb = p[24 + shift + 1];
                    dbm = (signed char)((int)pwdb - 110);
                }
                const unsigned char *f = p + pktOff;
                unsigned char fc0 = f[0];
                if (fc0 == 0x80 || fc0 == 0x50) {
                    (*totalBeacons)++;
                    ParseBeaconOrProbeResp(f, pktLen, ch, dbm);
                }
            }
        }

        unsigned long step = (pktOff + pktLen + 7UL) & ~7UL;
        if (step == 0) break;
        off += step;
    }
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */
void mainCRTStartup(void) {
    Print("========================================================\r\n");
    Print("  RTL8822BU Wi-Fi Radio Bring-Up & Scanner (Milestone 7)\r\n");
    Print("========================================================\r\n");

    g_hDev = CreateFileA("\\\\.\\RTWPROBE", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_hDev == INVALID_HANDLE_VALUE) {
        Print("[FATAL] Cannot open \\\\.\\RTWPROBE (err=");
        PrintDec(GetLastError());
        Print(")\r\nMake sure RTWPROBE.SYS is loaded on the USB dongle.\r\n");
        SaveLog();
        Print("Press ENTER to exit...");
        WaitForEnter();
        ExitProcess(1);
    }

    /* Stage 1: USB Pipes */
    Print("\r\n[1/7] Querying USB Pipes...\r\n");
    DWORD br = 0;
    if (!DeviceIoControl(g_hDev, IOCTL_RTW_GET_PIPES, NULL, 0, &g_Pipes, sizeof(g_Pipes), &br, NULL)) {
        Print("  [FATAL] IOCTL_RTW_GET_PIPES failed (err=");
        PrintDec(GetLastError());
        Print(") - Did you install the new Milestone 7 RTWPROBE.SYS and reboot?\r\n");
        CloseHandle(g_hDev);
        SaveLog();
        Print("Press ENTER to exit...");
        WaitForEnter();
        ExitProcess(1);
    }
    g_NumBulkOut = g_Pipes.NumBulkOut ? g_Pipes.NumBulkOut : 3;
    g_BulkOutMaxPkt = 64;
    for (unsigned long i = 0; i < g_Pipes.NumPipes && i < 8; i++) {
        if (!(g_Pipes.Pipes[i].EndpointAddress & 0x80) && g_Pipes.Pipes[i].PipeType == 2) {
            if (g_Pipes.Pipes[i].MaxPacketSize >= 64) {
                g_BulkOutMaxPkt = g_Pipes.Pipes[i].MaxPacketSize;
                break;
            }
        }
    }
    Print("  Pipes="); PrintDec(g_Pipes.NumPipes);
    Print(" BulkIN="); PrintHexN(g_Pipes.BulkInAddr, 2);
    Print(" BulkOUT("); PrintDec(g_NumBulkOut); Print(")=");
    for (unsigned long i = 0; i < g_NumBulkOut && i < 4; i++) {
        PrintHexN(g_Pipes.BulkOutAddrs[i], 2); Print(" ");
    }
    Print("MaxPkt="); PrintDec(g_BulkOutMaxPkt); Print("\r\n");

    /* Stage 2: Chip Version & EFUSE */
    Print("\r\n[2/7] Reading Chip Info & EFUSE...\r\n");
    unsigned long sysCfg1 = RegRead32(0x00F0);
    g_CutVer = (unsigned char)((sysCfg1 >> 12) & 0x0F);
    ReadEfuse();
    Print("  SYS_CFG1="); PrintHexN(sysCfg1, 8);
    Print(" Cut="); PrintDec(g_CutVer);
    Print(" RFE="); PrintDec(g_RfeOpt);
    Print(" Xtal="); PrintHexN(g_XtalK, 2);
    Print(" Pkg="); PrintDec(g_Pkg); Print("\r\n");
    Print("  Factory MAC="); PrintMac(g_Mac); Print("\r\n");

    /* Stage 3: MAC Power On */
    Print("\r\n[3/7] Powering On MAC (Linux rtw_mac_power_on)...\r\n");
    if (!RtwMacPowerOn()) {
        Print("  [FATAL] Power-on sequence failed!\r\n");
        CloseHandle(g_hDev);
        SaveLog();
        Print("Press ENTER to exit...");
        WaitForEnter();
        ExitProcess(1);
    }
    Print("  Power-On OK! REG_CR="); PrintHexN(RegRead8(0x0100), 2);
    Print(" SYS_FUNC_EN="); PrintHexN(RegRead16(0x0002), 4); Print("\r\n");

    /* Stage 4: Firmware Download */
    Print("\r\n[4/7] Downloading Firmware (WCPU_3081 Bulk+DDMA)...\r\n");
    if (!RtwDownloadFirmware()) {
        Print("  [WARN] Firmware boot did not reach 0xC078, continuing anyway...\r\n");
    }

    /* Stage 5: MAC / LLT / H2C / USB 1.1 RX Init */
    Print("\r\n[5/7] Initializing MAC, LLT, H2C & USB 64B Burst...\r\n");
    if (!RtwMacInit()) {
        Print("  [FATAL] MAC/LLT init failed!\r\n");
        CloseHandle(g_hDev);
        SaveLog();
        Print("Press ENTER to exit...");
        WaitForEnter();
        ExitProcess(1);
    }
    Print("  MAC/LLT OK! CR="); PrintHexN(RegRead8(0x0100), 2);
    Print(" RCR="); PrintHexN(RegRead32(0x0608), 8);
    Print(" RXDMA_MODE="); PrintHexN(RegRead8(0x0290), 2); Print("\r\n");

    /* Record idle RX FIFO counter baselines before BB/RF powers on */
    unsigned short idle2B0 = RegRead16(0x02B0);
    unsigned short idle284 = RegRead16(0x0284);
    Print("  Idle RX counters: R02B0="); PrintHexN(idle2B0, 4);
    Print(" R0284="); PrintHexN(idle284, 4); Print("\r\n");

    /* Stage 6: BB/RF Power-On & Table Load */
    Print("\r\n[6/7] Powering On BB/RF & Loading RTL8822B Tables...\r\n");
    RtwPhySetParam();
    Print("  RF_A[0x00]="); PrintHexN(RfRead(0, 0x00), 5);
    Print(" RF_A[0x18]="); PrintHexN(RfRead(0, 0x18), 5);
    Print(" RF_B[0x18]="); PrintHexN(RfRead(1, 0x18), 5); Print("\r\n");

    /* Stage 7: Live 2.4 GHz Channel Scan */
    static const unsigned char scanCh[] = { 1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10 };
    Print("\r\n[7/7] Scanning 2.4 GHz Channels (1,6,11,2..10)...\r\n");

    unsigned long totalUrbs = 0, totalFrames = 0, totalBeacons = 0, totalC2h = 0, totalCrcErr = 0;

    for (int ci = 0; ci < (int)(sizeof(scanCh) / sizeof(scanCh[0])); ci++) {
        unsigned char ch = scanCh[ci];
        RtwSetChannel2G(ch);
        Sleep(10);

        DWORD startTick = GetTickCount();
        unsigned long chFrames = 0;
        while ((GetTickCount() - startTick) < 450) {
            unsigned short c2b0 = RegRead16(0x02B0);
            unsigned short c284 = RegRead16(0x0284);
            if (c2b0 == idle2B0 && c284 == idle284) {
                Sleep(5);
                continue;
            }

            RTW_BULK_RX_REQ req;
            req.CheckFifo = 0;
            req.MaxLen    = 4096;
            br = 0;
            BOOL ok = DeviceIoControl(g_hDev, IOCTL_RTW_BULK_RX_RAW,
                                      &req, sizeof(req),
                                      &g_RxRaw, sizeof(g_RxRaw), &br, NULL);
            if (!ok || g_RxRaw.Length < 24) {
                Sleep(5);
                continue;
            }
            totalUrbs++;
            ProcessRxBuffer(g_RxRaw.Data, g_RxRaw.Length, ch,
                            &chFrames, &totalFrames, &totalBeacons, &totalC2h, &totalCrcErr);
        }
        Print("  CH"); PrintDec(ch);
        Print(": rx="); PrintDec(chFrames);
        Print(" (nets="); PrintDec((unsigned long)g_NetCount); Print(")\r\n");
    }

    /* Read BB Hardware CCA / CRC Counters for Radio Verification */
    unsigned long crcCck  = RegRead32(0x0F04);
    unsigned long crcOfdm = RegRead32(0x0F14);
    unsigned long ccaOfdm = RegRead32(0x0F08);
    unsigned long ccaCck  = RegRead32(0x0FCC);
    unsigned short r02b0  = RegRead16(0x02B0);
    unsigned long  r0284  = RegRead32(0x0284);

    /* Fallback: if BB received CRC-OK packets but FIFO counters stayed idle,
     * try direct Bulk IN reads on CH 1, 6, 11! */
    if (totalUrbs == 0 && ((crcCck & 0xFFFFUL) + (crcOfdm & 0xFFFFUL)) > 5) {
        static const unsigned char fbCh[] = { 1, 6, 11 };
        Print("\r\n  [!] BB heard "); PrintDec((crcCck & 0xFFFFUL) + (crcOfdm & 0xFFFFUL));
        Print(" CRC-OK frames while R02B0/R0284 stayed idle; trying direct Bulk IN...\r\n");
        for (int fi = 0; fi < 3; fi++) {
            unsigned char ch = fbCh[fi];
            unsigned long chFrames = 0;
            RtwSetChannel2G(ch);
            Sleep(30);
            for (int k = 0; k < 15; k++) {
                RTW_BULK_RX_REQ req;
                req.CheckFifo = 0;
                req.MaxLen    = 4096;
                br = 0;
                BOOL ok = DeviceIoControl(g_hDev, IOCTL_RTW_BULK_RX_RAW,
                                          &req, sizeof(req),
                                          &g_RxRaw, sizeof(g_RxRaw), &br, NULL);
                if (!ok || g_RxRaw.Length < 24) break;
                totalUrbs++;
                ProcessRxBuffer(g_RxRaw.Data, g_RxRaw.Length, ch,
                                &chFrames, &totalFrames, &totalBeacons, &totalC2h, &totalCrcErr);
            }
            Print("  Direct CH"); PrintDec(ch); Print(": rx="); PrintDec(chFrames); Print("\r\n");
        }
    }

    Print("\r\n========================================================\r\n");
    Print("  SCAN COMPLETE! Networks found: "); PrintDec((unsigned long)g_NetCount); Print("\r\n");
    Print("========================================================\r\n");
    static const char *secStr[] = { "OPEN", "WEP ", "WPA ", "WPA2" };
    for (int i = 0; i < g_NetCount; i++) {
        Print("  "); PrintDec((unsigned long)(i + 1)); Print(". ");
        Print(g_Nets[i].Ssid);
        Print("  [CH"); PrintDec(g_Nets[i].Channel);
        Print(" "); Print(secStr[g_Nets[i].Sec & 3]);
        Print(" "); PrintSigned(g_Nets[i].Dbm); Print("dBm ");
        PrintMac(g_Nets[i].Bssid);
        Print(" bcn="); PrintDec(g_Nets[i].Beacons); Print("]\r\n");
    }

    Print("\r\n[Radio Diagnostics]\r\n");
    Print("  USB_URBs="); PrintDec(totalUrbs);
    Print(" Frames="); PrintDec(totalFrames);
    Print(" Beacons="); PrintDec(totalBeacons);
    Print(" C2H="); PrintDec(totalC2h);
    Print(" CrcErr="); PrintDec(totalCrcErr); Print("\r\n");
    Print("  BB CCK_OK="); PrintDec(crcCck & 0xFFFF);
    Print(" CCK_ERR="); PrintDec(crcCck >> 16);
    Print(" OFDM_OK="); PrintDec(crcOfdm & 0xFFFF);
    Print(" OFDM_ERR="); PrintDec(crcOfdm >> 16); Print("\r\n");
    Print("  BB CCK_CCA="); PrintDec(ccaCck & 0xFFFF);
    Print(" OFDM_CCA="); PrintDec(ccaOfdm >> 16);
    Print(" R02B0="); PrintHexN(r02b0, 4);
    Print(" R0284="); PrintHexN(r0284, 8); Print("\r\n");

    CloseHandle(g_hDev);
    SaveLog();
    Print("\r\nSaved log to RTWSCAN.LOG. Press ENTER to exit...\r\n");
    WaitForEnter();
    ExitProcess(0);
}
