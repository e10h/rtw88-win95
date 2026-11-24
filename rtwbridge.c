/*
 * rtwbridge.c - Milestone 8 & 9: Real Wi-Fi (Open & WPA2-PSK AES-CCMP)
 * Ethernet <-> 802.11 Bridge for Windows 95 OSR 2.1 (Pentium MMX safe)
 *
 * Bridges live packets between:
 *   - \\.\RTW88.VXD (Win95 NDIS 3.1 VxD bound to VTCP.386 / VDHCP.386)
 *   - \\.\RTWPROBE  (WDM USB driver for Realtek RTL8822BU 802.11ac dongle)
 *
 * Usage:
 *   RTWBRIDGE.EXE "SSID"              (connect to Open Wi-Fi)
 *   RTWBRIDGE.EXE "SSID" "PASSWORD"   (connect to WPA2-PSK Wi-Fi)
 *   RTWBRIDGE.EXE                     (reads SSID & optional PASSWORD from WIFI.TXT)
 */
#include <windows.h>
#include "rtw8822b_tables.h"
#include "wpa2_crypto.h"

/* Exact 88-byte Windows 95 SHELL32.dll NOTIFYICONDATAA (V1 ABI) */
typedef struct _WIN95_NOTIFYICONDATAA {
    DWORD cbSize;
    HWND  hWnd;
    UINT  uID;
    UINT  uFlags;
    UINT  uCallbackMessage;
    HICON hIcon;
    char  szTip[64];
} WIN95_NOTIFYICONDATAA;

#define W95_NIM_ADD         0x00000000
#define W95_NIM_MODIFY      0x00000001
#define W95_NIM_DELETE      0x00000002
#define W95_NIF_MESSAGE     0x00000001
#define W95_NIF_ICON        0x00000002
#define W95_NIF_TIP         0x00000004
#define WM_TRAYICON         (WM_USER + 42)
#ifndef WM_DEVICECHANGE
#define WM_DEVICECHANGE     0x0219
#endif

#define IDC_STATUS          101
#define IDC_STATS           102
#define IDC_LOG             103
#define IDC_SSID            201
#define IDC_PASS            202
#define IDC_CONNECT         203
#define IDC_DISCONN         204
#define IDC_SHOWPASS        205
#define IDC_TOTRAY          206
#define IDC_BOOTWATCH       207
#define IDC_AUTOCONN        208
#define IDM_TRAY_OPEN       301
#define IDM_TRAY_EXIT       302

/* =========================================================================
 * IOCTLs: RTWPROBE.SYS & RTW88.VXD
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

#define RTW88_DIOC_GET_DIAG     1
#define RTW88_DIOC_POP_TX       2
#define RTW88_DIOC_INJECT_RX    3
#define RTW88_DIOC_SET_MAC      4

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

#pragma pack(push, 1)
typedef struct _RTW88_DIAG {
    DWORD Magic;
    DWORD InitCalls;
    DWORD NdisGetVerCarry;
    DWORD NdisVersion;
    DWORD DriverEntryCalls;
    DWORD WrapperHandle;
    DWORD RegMacStatus;
    DWORD MacHandle;
    DWORD AddAdapterCalls;
    DWORD RegAdapterStatus;
    DWORD AdapterHandle;
    DWORD OpenAdapterCalls;
    DWORD TxEnqueued;
    DWORD TxDequeued;
    DWORD TxDropped;
    DWORD RxInjected;
    DWORD TransferDataCalls;
    DWORD PktMemAddr;
    BYTE  CurrentMac[6];
    BYTE  Reserved[2];
} RTW88_DIAG;
#pragma pack(pop)

/* =========================================================================
 * Global State & Buffers (Static - zero malloc)
 * ========================================================================= */
static RTW_REG_BATCH    g_Batch;
static RTW_BULK_RX_RAW  g_RxRaw;
static RTW_BULK_TX_PIPE g_TxMain;
static RTW_PIPES_RESP   g_Pipes;
static unsigned char    g_FwBuf[196608];
static unsigned char    g_LogMap[768];
static unsigned char    g_EthTxBuf[1600];
static unsigned char    g_EthRxBuf[1600];
static unsigned char    g_DecBuf[2048];
static unsigned char    g_FrameMain[2048];
static unsigned char    g_PlainMain[1600];

static HANDLE           g_hDev    = INVALID_HANDLE_VALUE;
static HANDLE           g_hVxd    = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_Lock;
static CRITICAL_SECTION g_PrintLock;
static CRITICAL_SECTION g_VxdLock;
static CRITICAL_SECTION g_TxLock;
static volatile int     g_Running = 1;
static u8               g_ForceCh = 0;

/* Native Win95 GUI Handles & Display Buffers */
static HWND             g_hWnd       = NULL;
static HWND             g_hStatus    = NULL;
static HWND             g_hStats     = NULL;
static HWND             g_hEdit      = NULL;
static HWND             g_hSsidEdit  = NULL;
static HWND             g_hPassEdit  = NULL;
static HWND             g_hBtnConn   = NULL;
static HWND             g_hBtnDisc   = NULL;
static HWND             g_hChkShow   = NULL;
static HWND             g_hBtnTray   = NULL;
static HWND             g_hChkBoot   = NULL;
static HWND             g_hChkAuto   = NULL;
static HICON            g_hAppIcon   = NULL;
static char             g_StatusText[160];
static char             g_DispBuf[14000];
static volatile int     g_LogDirty     = 0;
static volatile int     g_StatusDirty  = 1;
static volatile int     g_WorkerActive = 0;
static volatile int     g_BridgeStop   = 0;
static volatile int     g_DongleSeen   = 0;
static HANDLE           g_hWorker      = NULL;
static int              g_StartInTray  = 0;
static int              g_AutoConnect  = 0;
static int              g_CliConnect   = 0;
static int              g_TrayAdded    = 0;

/* Hardware Info */
static u8  g_CutVer        = 0;
static u8  g_RfeOpt        = 2;
static u8  g_XtalK         = 0x20;
static u8  g_Pkg           = 0x0F;
static u8  g_StaMac[6]     = { 0x80, 0x42, 0xEF, 0x68, 0x80, 0x42 };
static u32 g_NumBulkOut    = 3;
static u32 g_BulkOutMaxPkt = 64;
static u32 g_MgmtPipeIdx   = 0;
static u32 g_DataPipeIdx   = 2;
static u16 g_RsvdBoundary  = 1996;
static u16 g_RsvdH2cqAddr  = 2036;

/* Target AP & Wi-Fi State */
static char g_TargetSsid[36];
static u32  g_TargetSsidLen = 0;
static char g_Password[68];
static u32  g_PasswordLen   = 0;
static u8   g_Bssid[6];
static u8   g_ApChannel     = 0;
static int  g_ApIsWpa2      = 0;
static u8   g_ApGroupCipher[4] = { 0x00, 0x0F, 0xAC, 0x04 }; /* Default CCMP */
static u8   g_RsnIe[22];
static u16  g_Aid           = 0;
static u16  g_TxSeq         = 1;

/* WPA2-PSK Keys */
static u8   g_Pmk[32];
static u8   g_SNonce[32];
static u8   g_ANonce[32];
static u8   g_Ptk[48];      /* KCK[0..15], KEK[16..31], TK[32..47] */
static u8   g_Gtk[4][16];
static int  g_GtkValid[4]   = { 0, 0, 0, 0 };
static volatile int g_Encrypted = 0;
static int  g_HandshakeState = 0; /* 0=wait M1, 1=sent M2, 2=done */
static u32  g_TxPnLo        = 1;
static u32  g_TxPnHi        = 0;

/* Live Bridge Counters & Active Data TX Mode */
static volatile u32 g_StatTxEth   = 0;
static volatile u32 g_StatRxEth   = 0;
static volatile u32 g_StatRxBcn   = 0;
static volatile u32 g_StatDecFail = 0;
static volatile u32 g_StatEapol   = 0;
static volatile u32 g_StatTxEcho  = 0;
static volatile u32 g_StatRxUcast = 0;

static u32 g_DataQsel   = 0U;   /* Auto-selected by TX Self-Test */
static u32 g_DataRate   = 0U;   /* 0=1M CCK, 4=6M OFDM */
static u32 g_DataRateId = 8U;   /* 8=B_20M, 6=G */
static int g_DataHwSeq  = 0;    /* 0=SW seq, 1=HW seq */

/* Log Buffer */
#define LOG_MAX 32768
static char g_LogBuf[LOG_MAX];
static u32  g_LogLen = 0;

/* =========================================================================
 * GUI Log, Status & String Helpers (No Libc, Win95 Safe)
 * ========================================================================= */
static u32 StrLen(const char *s) {
    const volatile char *p = (const volatile char *)s;
    u32 n = 0;
    while (p[n]) n++;
    return n;
}

static void StrAppend(char *dst, u32 maxLen, const char *src) {
    u32 d = StrLen(dst);
    for (u32 i = 0; src[i] && d + 1 < maxLen; i++) dst[d++] = src[i];
    dst[d] = '\0';
}

static void StrAppendDec(char *dst, u32 maxLen, unsigned long v) {
    char t[16], b[16];
    int n = 0, j = 0;
    if (v == 0) { StrAppend(dst, maxLen, "0"); return; }
    while (v > 0) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) b[j++] = t[--n];
    b[j] = '\0';
    StrAppend(dst, maxLen, b);
}

static void SetStatus(const char *s) {
    EnterCriticalSection(&g_PrintLock);
    g_StatusText[0] = '\0';
    StrAppend(g_StatusText, sizeof(g_StatusText), s);
    g_StatusDirty = 1;
    LeaveCriticalSection(&g_PrintLock);
}

static void Print(const char *s) {
    u32 len = StrLen(s);
    if (!len) return;
    EnterCriticalSection(&g_PrintLock);
    /* Rolling 32 KB log buffer so long sessions never stop logging */
    if (g_LogLen + len + 1 >= LOG_MAX) {
        u32 keep = LOG_MAX / 2;
        if (keep < g_LogLen) {
            u32 drop = g_LogLen - keep;
            while (drop < g_LogLen && g_LogBuf[drop] != '\n') drop++;
            if (drop < g_LogLen) drop++;
            u32 rem = g_LogLen - drop;
            for (u32 i = 0; i < rem; i++) g_LogBuf[i] = g_LogBuf[drop + i];
            g_LogLen = rem;
        } else {
            g_LogLen = 0;
        }
    }
    for (u32 i = 0; i < len && g_LogLen + 1 < LOG_MAX; i++) {
        g_LogBuf[g_LogLen++] = s[i];
    }
    g_LogBuf[g_LogLen] = '\0';
    g_LogDirty = 1;
    LeaveCriticalSection(&g_PrintLock);
}

static void PrintHex2(u8 v) {
    static const char hx[] = "0123456789ABCDEF";
    char b[3];
    b[0] = hx[(v >> 4) & 0xF];
    b[1] = hx[v & 0xF];
    b[2] = '\0';
    Print(b);
}

static void PrintDec(unsigned long v) {
    char t[16], b[16];
    int n = 0, j = 0;
    if (v == 0) { Print("0"); return; }
    while (v > 0) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) b[j++] = t[--n];
    b[j] = '\0';
    Print(b);
}

static void PrintMac(const u8 *m) {
    for (int i = 0; i < 6; i++) {
        PrintHex2(m[i]);
        if (i < 5) Print(":");
    }
}

static void PrintIp(const u8 *ip) {
    PrintDec(ip[0]); Print(".");
    PrintDec(ip[1]); Print(".");
    PrintDec(ip[2]); Print(".");
    PrintDec(ip[3]);
}

static void BuildExeDirPath(char *out, u32 maxLen, const char *name) {
    char exe[260];
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof(exe) - 1);
    out[0] = '\0';
    if (len > 0 && len < sizeof(exe)) {
        exe[len] = '\0';
        int last = -1;
        for (int i = 0; exe[i]; i++) {
            if (exe[i] == '\\' || exe[i] == '/') last = i;
        }
        if (last >= 0) {
            exe[last + 1] = '\0';
            StrAppend(out, maxLen, exe);
        }
    }
    StrAppend(out, maxLen, name);
}

static HANDLE OpenLocalFileRead(const char *name) {
    HANDLE h = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) return h;
    char full[300];
    BuildExeDirPath(full, sizeof(full), name);
    return CreateFileA(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
}

static void SaveLog(void) {
    char path[300];
    BuildExeDirPath(path, sizeof(path), "RTWBRIDG.LOG");
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileA("RTWBRIDG.LOG", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        EnterCriticalSection(&g_PrintLock);
        WriteFile(h, g_LogBuf, g_LogLen, &w, NULL);
        LeaveCriticalSection(&g_PrintLock);
        CloseHandle(h);
    }
}

/* =========================================================================
 * Register & Batched I/O (Proven Milestone 7 Layer)
 * ========================================================================= */
static void BatchFlush(void) {
    if (g_Batch.Count == 0) return;
    DWORD br = 0;
    DWORD inLen = (DWORD)(sizeof(unsigned long) + g_Batch.Count * sizeof(RTW_REG_OP));
    DeviceIoControl(g_hDev, IOCTL_RTW_REG_BATCH, &g_Batch, inLen, NULL, 0, &br, NULL);
    g_Batch.Count = 0;
}

static void BatchPush(u8 op, u8 width, u16 addr, u32 mask, u32 val) {
    if (g_Batch.Count >= 256) BatchFlush();
    RTW_REG_OP *e = &g_Batch.Ops[g_Batch.Count++];
    e->Op = op; e->Width = width; e->Addr = addr; e->Mask = mask; e->Value = val;
}

static u32 RegReadW(u16 addr, u16 width) {
    BatchFlush();
    RTW_REG_REQ req;
    req.Addr = addr; req.Width = width; req.Value = 0; req.Status = 0;
    DWORD br = 0;
    DeviceIoControl(g_hDev, IOCTL_RTW_REG_READ, &req, sizeof(req), &req, sizeof(req), &br, NULL);
    return (u32)req.Value;
}

static u8  RegRead8(u16 a)  { return (u8)RegReadW(a, 1); }
static u16 RegRead16(u16 a) { return (u16)RegReadW(a, 2); }
static u32 RegRead32(u16 a) { return RegReadW(a, 4); }

static void RegWrite8(u16 a, u8 v)   { BatchPush(RTW_OP_WRITE, 1, a, 0xFF, v); }
static void RegWrite16(u16 a, u16 v) { BatchPush(RTW_OP_WRITE, 2, a, 0xFFFF, v); }
static void RegWrite32(u16 a, u32 v) { BatchPush(RTW_OP_WRITE, 4, a, 0xFFFFFFFFU, v); }

static u32 MaskShift(u32 m) {
    u32 s = 0;
    if (!m) return 0;
    while (!(m & 1U)) { m >>= 1; s++; }
    return s;
}

static void RegMask8(u16 a, u8 m, u8 v)    { BatchPush(RTW_OP_MASK, 1, a, m, ((u32)v << MaskShift(m)) & m); }
static void RegMask32(u16 a, u32 m, u32 v) { BatchPush(RTW_OP_MASK, 4, a, m, (v << MaskShift(m)) & m); }
static void RegSet8(u16 a, u8 b)           { BatchPush(RTW_OP_MASK, 1, a, b, b); }
static void RegClr8(u16 a, u8 b)           { BatchPush(RTW_OP_MASK, 1, a, b, 0); }
static void RegSet16(u16 a, u16 b)         { BatchPush(RTW_OP_MASK, 2, a, b, b); }
static void RegSet32(u16 a, u32 b)         { BatchPush(RTW_OP_MASK, 4, a, b, b); }
static void RegClr32(u16 a, u32 b)         { BatchPush(RTW_OP_MASK, 4, a, b, 0); }
static void RegMask32S(u16 a, u32 m, u32 v){ RegMask32(a, m, v); RegMask32((u16)(a + 0x200), m, v); }

static u32 RegReadMask32(u16 a, u32 m) { return (RegRead32(a) & m) >> MaskShift(m); }

static u32 RfRead(u8 path, u16 rfAddr) {
    return RegRead32((u16)(((path == 0) ? 0x2800 : 0x2C00) + ((rfAddr & 0xFF) << 2))) & 0xFFFFFU;
}

static void RfWrite(u8 path, u16 rfAddr, u32 m, u32 v) {
    m &= 0xFFFFFU;
    BatchPush(RTW_OP_RF_WRITE, path, rfAddr, m, (v << MaskShift(m)) & m);
}

static int PollReg8(u16 a, u8 m, u8 t, int maxMs) {
    BatchFlush();
    DWORD s = GetTickCount();
    t &= m;
    for (;;) {
        if ((RegRead8(a) & m) == t) return 1;
        if ((int)(GetTickCount() - s) >= maxMs) return 0;
        Sleep(1);
    }
}

static int PollReg16(u16 a, u16 m, u16 t, int maxMs) {
    BatchFlush();
    DWORD s = GetTickCount();
    t &= m;
    for (;;) {
        if ((RegRead16(a) & m) == t) return 1;
        if ((int)(GetTickCount() - s) >= maxMs) return 0;
        Sleep(1);
    }
}

static int PollReg32(u16 a, u32 m, u32 t, int maxMs) {
    BatchFlush();
    DWORD s = GetTickCount();
    t &= m;
    for (;;) {
        if ((RegRead32(a) & m) == t) return 1;
        if ((int)(GetTickCount() - s) >= maxMs) return 0;
        Sleep(1);
    }
}

/* =========================================================================
 * RTL8822BU Bring-Up (EFUSE, PowerOn, FW, MAC/LLT, PHY/RF, TX Power)
 * ========================================================================= */
static u8 EfuseReadPhyByte(u16 addr, int *ok) {
    u32 ctl = RegRead32(0x0030);
    ctl &= ~(0xFFU | (0x3FFU << 8));
    ctl |= ((u32)(addr & 0x3FF) << 8);
    RegWrite32(0x0030, ctl & ~0x80000000U);
    BatchFlush();
    for (int i = 0; i < 200; i++) {
        ctl = RegRead32(0x0030);
        if (ctl & 0x80000000U) { *ok = 1; return (u8)(ctl & 0xFF); }
    }
    *ok = 0;
    return 0xFF;
}

static void ReadEfuse(void) {
    for (u32 i = 0; i < sizeof(g_LogMap); i++) g_LogMap[i] = 0xFF;
    RegMask32(0x0034, 0x300U, 0);
    RegClr8(0x0037, 0x80);
    BatchFlush();

    u16 phyIdx = 0, maxPhy = 1024 - 96;
    int ok = 1;
    while (phyIdx < maxPhy) {
        u8 h1 = EfuseReadPhyByte(phyIdx, &ok);
        if (!ok || h1 == 0xFF) break;
        u8 blk, wen;
        if ((h1 & 0x1F) == 0x0F) {
            u8 h2 = EfuseReadPhyByte((u16)(phyIdx + 1), &ok);
            if (!ok || h2 == 0xFF) break;
            blk = (u8)(((h2 & 0xF0) >> 1) | ((h1 >> 5) & 7));
            wen = (u8)(h2 & 0x0F);
            phyIdx += 2;
        } else {
            blk = (u8)((h1 & 0xF0) >> 4);
            wen = (u8)(h1 & 0x0F);
            phyIdx += 1;
        }
        for (int i = 0; i < 4; i++) {
            if (wen & (1 << i)) continue;
            u16 li = (u16)(((u16)blk << 3) + (i << 1));
            if (phyIdx + 2 > maxPhy || (u32)(li + 2) > sizeof(g_LogMap)) { phyIdx = maxPhy; break; }
            u8 b0 = EfuseReadPhyByte(phyIdx++, &ok);
            u8 b1 = EfuseReadPhyByte(phyIdx++, &ok);
            if (!ok) { phyIdx = maxPhy; break; }
            g_LogMap[li] = b0; g_LogMap[li + 1] = b1;
        }
    }

    g_XtalK  = (g_LogMap[0xB9] == 0xFF) ? 0x20 : g_LogMap[0xB9];
    g_RfeOpt = g_LogMap[0xCA];
    g_Pkg    = (g_LogMap[0x1FB] == 0xFF) ? 0 : g_LogMap[0x1FB];
    if (g_RfeOpt != 2 && g_RfeOpt != 3 && g_RfeOpt != 5) g_RfeOpt = 2;
}

typedef struct { u16 off; u8 cut, intf, cmd, mask, val; } PWR_CMD;
static const PWR_CMD g_DisToEmu[] = {
    {0x004A,0xFF,2,0,0x01,0x00}, {0x0005,0xFF,0xF,0,0x98,0x00}, {0xFFFF,0,0,3,0,0}
};
static const PWR_CMD g_EmuToAct[] = {
    {0x0012,0xFF,0xF,0,0x02,0x00}, {0x0012,0xFF,0xF,0,0x01,0x01},
    {0x0020,0xFF,2,  0,0x01,0x01}, {0x0001,0xFF,2,  2,1,   1   },
    {0x0000,0xFF,2,  0,0x20,0x00}, {0x0005,0xFF,0xF,0,0x1C,0x00},
    {0x0006,0xFF,0xF,1,0x02,0x02}, {0xFF1A,0xFF,2,  0,0xFF,0x00},
    {0x0006,0xFF,0xF,0,0x01,0x01}, {0x0005,0xFF,0xF,0,0x80,0x00},
    {0x0005,0xFF,0xF,0,0x18,0x00}, {0x10C3,0xFF,2,  0,0x01,0x01},
    {0x0005,0xFF,0xF,0,0x01,0x01}, {0x0005,0xFF,0xF,1,0x01,0x00},
    {0x0020,0xFF,0xF,0,0x08,0x08}, {0x10A8,0x08,0xF,0,0xFF,0x00},
    {0x10A9,0x08,0xF,0,0xFF,0xEF}, {0x10AA,0x08,0xF,0,0xFF,0x0C},
    {0x0029,0xFF,0xF,0,0xFF,0xF9}, {0x0024,0xFF,0xF,0,0x04,0x00},
    {0x00AF,0xFF,0xF,0,0x20,0x20}, {0xFFFF,0,0,3,0,0}
};
static const PWR_CMD g_ActToEmu[] = {
    {0x0093,0xFF,0xF,0,0x08,0x00}, {0x001F,0xFF,0xF,0,0xFF,0x00},
    {0x00EF,0xFF,0xF,0,0xFF,0x00}, {0xFF1A,0xFF,2,  0,0xFF,0x30},
    {0x0049,0xFF,0xF,0,0x02,0x00}, {0x0006,0xFF,0xF,0,0x01,0x01},
    {0x0002,0xFF,0xF,0,0x02,0x00}, {0x10C3,0xFF,2,  0,0x01,0x00},
    {0x0005,0xFF,0xF,0,0x02,0x02}, {0x0005,0xFF,0xF,1,0x02,0x00},
    {0x0020,0xFF,0xF,0,0x08,0x00}, {0x0000,0xFF,2,  0,0x20,0x20},
    {0xFFFF,0,0,3,0,0}
};
static const PWR_CMD g_EmuToDis[] = {
    {0x0007,0xFF,2,  0,0xFF,0x20}, {0x0067,0xFF,0xF,0,0x20,0x00},
    {0x004A,0xFF,2,  0,0x01,0x00}, {0x0081,0xFF,0xF,0,0xC0,0x00},
    {0x0005,0xFF,2,  0,0x18,0x08}, {0x0090,0xFF,2,  0,0x02,0x00},
    {0xFFFF,0,0,3,0,0}
};

static int RunPwrSeq(const PWR_CMD *seq) {
    u8 cm = (u8)(1U << (g_CutVer + 1));
    for (const PWR_CMD *c = seq; c->cmd != 3; c++) {
        if (!(c->intf & 2) || !(c->cut & cm)) continue;
        if (c->cmd == 0) BatchPush(RTW_OP_MASK, 1, c->off, c->mask, c->val & c->mask);
        else if (c->cmd == 1) { if (!PollReg8(c->off, c->mask, c->val, 500)) return 0; }
        else if (c->cmd == 2) { BatchFlush(); Sleep(c->off ? c->off : 1); }
    }
    BatchFlush();
    return 1;
}

static void PreSysCfg(void) {
    RegWrite8(0x001C, 0);
    RegSet32(0x0064, (1U << 29) | (1U << 28));
    RegClr32(0x004C, (1U << 26) | (1U << 25));
    RegSet32(0x0040, (1U << 2));
    RegClr8(0x0002, 0x03);
    RegClr8(0x001F, 0x07);
    RegClr32(0x00EC, 0x07000000U);
    BatchFlush();
}

static int PowerSwitch(int on) {
    u8 rpwm = RegRead8(0xFE58);
    if (RegRead16(0x0080) == 0xC078) {
        RegWrite8(0xFE58, (u8)((rpwm ^ 0x80) & 0x80));
        BatchFlush();
    }
    int cur = (RegRead8(0x0100) == 0xEA || (RegRead8(0x00F5) & 1)) ? 0 : 1;
    if (on == cur) return -1;
    if (on) {
        if (!RunPwrSeq(g_DisToEmu) || !RunPwrSeq(g_EmuToAct)) return 0;
        RegClr8(0x00F5, 0x01);
        BatchFlush();
    } else {
        if (!RunPwrSeq(g_ActToEmu) || !RunPwrSeq(g_EmuToDis)) return 0;
    }
    return 1;
}

static int RtwMacPowerOn(void) {
    PreSysCfg();
    int r = PowerSwitch(1);
    if (r == -1) {
        PowerSwitch(0);
        Sleep(50);
        PreSysCfg();
        r = PowerSwitch(1);
    }
    if (r != 1) return 0;
    RegSet32(0x1080, (1U << 16) | (1U << 8));
    RegSet8(0x0003, 0xDC);
    RegMask8(0x1103, 0x0F, 0x0C);
    BatchFlush();
    u32 tmp = RegRead32(0x0080);
    if (tmp & (1U << 20)) {
        RegWrite32(0x0080, tmp & ~(1U << 20));
        RegClr32(0x0040, (1U << 19));
        BatchFlush();
    }
    return 1;
}

static u16 CalcTxDescChecksum(const u8 *desc) {
    u16 sum = 0;
    const u16 *w = (const u16 *)desc;
    for (int i = 0; i < 16; i++) sum ^= w[i];
    return sum;
}

static int SendFwPkt(const u8 *data, u32 size) {
    u32 txSize = size;
    if (((txSize + 48) & (g_BulkOutMaxPkt - 1)) == 0 || ((txSize + 48) & 511) == 0) txSize += 1;

    u8 bcn = RegRead8(0x0550);
    RegWrite16(0x0204, 0x8000);
    u8 cr1 = RegRead8(0x0101);
    RegWrite8(0x0101, (u8)(cr1 | 1));
    RegWrite8(0x0550, (u8)((bcn & ~0x08) | 0x10));
    BatchFlush();

    c_zero(g_TxMain.Data, 48 + txSize);
    u32 *dw = (u32 *)g_TxMain.Data;
    dw[0] = (txSize & 0xFFFFU) | (48U << 16) | (1U << 26);
    dw[1] = (16U << 8);
    dw[7] = CalcTxDescChecksum(g_TxMain.Data);
    c_copy(g_TxMain.Data + 48, data, size);

    g_TxMain.OutPipeIdx = 0;
    g_TxMain.Length = 48 + txSize;
    DWORD br = 0;
    BOOL ok = DeviceIoControl(g_hDev, IOCTL_RTW_BULK_TX_PIPE, &g_TxMain, 8 + g_TxMain.Length, NULL, 0, &br, NULL);
    int rdy = ok ? PollReg16(0x0204, 0x8000, 0x8000, 500) : 0;

    RegWrite16(0x0204, (u16)(g_RsvdBoundary | 0x8000));
    RegWrite8(0x0550, bcn);
    RegWrite8(0x0101, cr1);
    BatchFlush();
    return (ok && rdy) ? 1 : 0;
}

static int DownloadFwSeg(const u8 *data, u32 dst, u32 size) {
    u32 off = 0, rem = size;
    int first = 1;
    RegSet32(0x1208, (1U << 25));
    BatchFlush();
    while (rem > 0) {
        u32 pkt = (rem >= 4096) ? 4096 : rem;
        if (!SendFwPkt(data + off, pkt)) return 0;
        if (!PollReg32(0x1208, (1U << 31), 0, 500)) return 0;
        u32 ctrl = (1U << 31) | (1U << 29) | (pkt & 0x3FFFFU) | (first ? 0 : (1U << 24));
        RegWrite32(0x1200, 0x18780000U + 48U);
        RegWrite32(0x1204, dst + off);
        RegWrite32(0x1208, ctrl);
        if (!PollReg32(0x1208, (1U << 31), 0, 500)) return 0;
        first = 0; off += pkt; rem -= pkt;
    }
    if (RegRead32(0x1208) & (1U << 27)) return 0;
    u8 fc = RegRead8(0x0080);
    fc |= (dst < 0x00200000U) ? (u8)0x18 : (u8)0x60;
    RegWrite8(0x0080, fc);
    BatchFlush();
    return 1;
}

static int RtwDownloadFirmware(void) {
    HANDLE hFw = OpenLocalFileRead("rtw8822b_fw.bin");
    if (hFw == INVALID_HANDLE_VALUE) { Print("  [ERR] rtw8822b_fw.bin not found!\r\n"); return 0; }
    DWORD fwSz = GetFileSize(hFw, NULL), br = 0;
    if (fwSz < 64 || fwSz > sizeof(g_FwBuf)) { CloseHandle(hFw); return 0; }
    ReadFile(hFw, g_FwBuf, fwSz, &br, NULL);
    CloseHandle(hFw);

    u32 dAddr = (*(u32 *)(g_FwBuf + 0x20)) & ~0x80000000U;
    u32 dSz   = (*(u32 *)(g_FwBuf + 0x24)) + 8U;
    u32 iSz   = (*(u32 *)(g_FwBuf + 0x30)) + 8U;
    u32 eSz   = (g_FwBuf[0x18] & 0x10) ? ((*(u32 *)(g_FwBuf + 0x34)) + 8U) : 0;
    u32 eAddr = (*(u32 *)(g_FwBuf + 0x38)) & ~0x80000000U;
    u32 iAddr = (*(u32 *)(g_FwBuf + 0x3C)) & ~0x80000000U;
    if (64U + dSz + iSz + eSz != fwSz) return 0;

    u32 lteBckp = 0;
    RegWrite32(0x1700, 0x800F0038U);
    int hasLte = PollReg32(0x1700, (1U << 29), (1U << 29), 50);
    if (hasLte) lteBckp = RegRead32(0x1708);

    RegClr8(0x0003, 0x04); RegClr8(0x001D, 0x01); BatchFlush();

    u8  bk_pq1 = RegRead8(0x010D); RegWrite8(0x010D, 0xC0);
    u8  bk_cr  = RegRead8(0x0100); RegWrite8(0x0100, 0x05); RegWrite32(0x1330, 0x80000000U);
    u16 bk_fp1 = RegRead16(0x0230);
    u32 bk_rq2 = RegRead32(0x022C) | 0x80000000U;
    RegWrite16(0x0230, 0x0200); RegWrite32(0x022C, bk_rq2);
    u8  bk_bcn = RegRead8(0x0550); RegWrite8(0x0550, (u8)((bk_bcn & ~0x08) | 0x10));
    BatchFlush();

    RegClr8(0x1082, 1); RegClr8(0x0009, 0x40);
    RegSet8(0x1082, 1); RegSet8(0x0009, 0x40);
    BatchFlush();
    RegWrite16(0x0080, (u16)((RegRead16(0x0080) & 0x3800) | 1));
    BatchFlush();

    int ok = DownloadFwSeg(g_FwBuf + 64, dAddr, dSz);
    if (ok) ok = DownloadFwSeg(g_FwBuf + 64 + dSz, iAddr, iSz);
    if (ok && eSz) ok = DownloadFwSeg(g_FwBuf + 64 + dSz + iSz, eAddr, eSz);

    RegWrite8(0x010D, bk_pq1); RegWrite8(0x0100, bk_cr); RegWrite32(0x1330, 0x80000000U);
    RegWrite16(0x0230, bk_fp1); RegWrite32(0x022C, bk_rq2); RegWrite8(0x0550, bk_bcn);
    BatchFlush();
    if (!ok) return 0;

    RegWrite32(0x0210, 4); BatchFlush();
    u16 fc = RegRead16(0x0080);
    if ((fc & 0x0050) != 0x0050) return 0;
    RegWrite16(0x0080, (u16)((fc | 0x4000) & ~1));
    RegSet8(0x001D, 1); RegSet8(0x0003, 4);
    BatchFlush();
    if (hasLte) {
        RegWrite32(0x1704, lteBckp);
        RegWrite32(0x1700, 0xC00F0038U);
        PollReg32(0x1700, (1U << 29), (1U << 29), 50);
    }
    return PollReg16(0x0080, 0xCFFF, 0xC078, 1500);
}

static u32 DmaMapToPipe(u8 m) {
    /* HIGH(3)->0, NORMAL(2)->1, LOW(1)->2, EXTRA(0)->3 */
    if (m == 3) return 0;
    if (m == 2) return 1;
    if (m == 1) return 2;
    return 3;
}

static int RtwMacInit(void) {
    u8 vo, vi, be, bk, mg, hi;
    u16 hq, lq, nq, exq, gapq;
    if (g_NumBulkOut == 2) {
        vo=2; vi=2; be=3; bk=3; mg=3; hi=3; hq=64; lq=64; nq=0; exq=0; gapq=1;
    } else if (g_NumBulkOut == 3) {
        vo=2; vi=2; be=3; bk=1; mg=3; hi=3; hq=64; lq=64; nq=64; exq=0; gapq=1;
    } else {
        vo=2; vi=2; be=3; bk=1; mg=3; hi=3; hq=64; lq=64; nq=64; exq=64; gapq=1;
    }
    g_MgmtPipeIdx = DmaMapToPipe(mg);
    /* Map BE (TID0) and MGMT both to HIGH queue -> proven Bulk OUT Pipe 0 */
    g_DataPipeIdx = DmaMapToPipe(be);

    u16 pqMap = (u16)((vo << 4) | (vi << 6) | (be << 8) | (bk << 10) | (mg << 12) | (hi << 14));
    RegWrite16(0x010C, pqMap);
    RegWrite8(0x0100, 0x00);
    RegWrite8(0x0100, 0xFF);
    RegClr8(0x0101, 0x02); /* Ensure BIT_MAC_SEC_EN (HW crypto) is OFF */
    RegWrite16(0x0680, 0x0000); /* REG_SECCFG = 0 (pure software AES-CCMP) */
    RegWrite32(0x1330, 0x80000000U);
    RegSet8(0x010C, 0x01);

    g_RsvdBoundary = 2048 - 52;
    g_RsvdH2cqAddr = 2048 - 4 - 8;
    u16 pubq = (u16)(g_RsvdBoundary - hq - lq - nq - exq - gapq);
    RegWrite16(0x0230, hq); RegWrite16(0x0234, lq);
    RegWrite16(0x0238, nq); RegWrite16(0x023C, exq);
    RegWrite16(0x0240, pubq);
    RegSet32(0x022C, 0x80000000U);

    RegWrite16(0x0204, g_RsvdBoundary);
    RegSet8(0x0422, 0x10);
    RegWrite16(0x0424, g_RsvdBoundary);
    RegWrite16(0x0206, g_RsvdBoundary);
    RegWrite16(0x0456, g_RsvdBoundary);
    RegWrite32(0x011C, 24576U - 256U - 1U);

    RegMask8(0x0208, 0xF0, 3);
    RegWrite8(0x020B, 3);
    RegSet8(0x020D, 0x02);
    RegSet8(0x0208, 0x01);
    if (!PollReg8(0x0208, 0x01, 0x00, 500)) return 0;
    RegWrite8(0x0103, 0x00);

    u32 h2cA = (u32)g_RsvdH2cqAddr << 7, h2cS = 8U << 7;
    RegMask32(0x0244, 0x3FFFFU, h2cA);
    RegMask32(0x024C, 0x3FFFFU, h2cA);
    RegMask32(0x0248, 0x3FFFFU, h2cA + h2cS);
    RegMask8(0x0254, 0x03, 0x01);
    RegMask8(0x0254, 0x04, 0x01);
    RegSet8(0x020D, 0x80);

    RegClr8(0x04BC, 0x40); RegWrite8(0x0455, 0x70); RegSet8(0x045E, 0x04);
    RegWrite32(0x04C8, 0x202008FFU); RegWrite16(0x04CE, 0x0801);
    RegWrite8(0x1448, 6); RegWrite8(0x144A, 6); RegWrite8(0x144C, 6); RegWrite8(0x144E, 6);
    RegClr8(0x05B4, 0x70); RegWrite16(0x0522, 0);
    RegWrite32(0x04D0, 0); RegWrite32(0x04D4, 0); /* MACID 0 awake & not dropping TX */
    RegWrite8(0x051B, 0x09); RegWrite8(0x0512, 0x19);
    RegWrite32(0x0514, 0x10100E0AU);
    RegWrite16(0x0502, 0x0186); RegWrite16(0x0506, 0x03BC);
    RegWrite32(0x0544, 0x001B0005U); RegWrite16(0x055E, 0x3030);
    RegSet8(0x0550, 0x08); RegWrite32(0x0540, 0x00006404U);
    RegWrite8(0x0558, 0x04); RegWrite8(0x0559, 0x02); RegClr8(0x0521, 0x10);

    RegWrite16(0x06A0, 0xFFFF); RegWrite16(0x06A2, 0x0FFF); RegWrite16(0x06A4, 0xFFFF);
    RegWrite32(0x0608, 0xE400220FU);
    RegWrite8(0x060C, 24); RegWrite8(0x0606, 0x30); RegWrite8(0x0605, 0x30);
    RegWrite32(0x07D8, 0xB0810041U); RegWrite8(0x07D4, 0x98); RegSet8(0x0718, 0x40);

    RegWrite8(0x060F, 4);
    RegMask8(0x0115, 0x0F, 0x0F);
    RegSet32(0x0608, (1U << 28));
    RegClr32(0x07D4, (1U << 8) | (1U << 9));

    u8 burstSz = (g_BulkOutMaxPkt <= 64) ? 2 : 1;
    RegWrite8(0x0290, (u8)(0x0E | (burstSz << 4)));
    RegSet16(0x020C, (1U << 9));
    RegSet8(0x010C, 0x04);
    RegClr8(0x0283, 0x80);
    RegWrite16(0x0280, 0x0100);
    BatchFlush();
    return 1;
}

static int CheckPhyCond(unsigned long c) {
    u8 dc = g_CutVer ? g_CutVer : 15;
    u8 dp = g_Pkg ? g_Pkg : 15;
    u8 cut = (u8)((c >> 24) & 0xF), pkg = (u8)((c >> 12) & 0xF);
    u8 intf = (u8)((c >> 8) & 0xF), rfe = (u8)(c & 0xFF);
    if (cut && cut != dc) return 0;
    if (pkg && pkg != dp) return 0;
    if (intf && intf != 2) return 0;
    if (rfe != g_RfeOpt) return 0;
    return 1;
}

static void LoadPhyTable(const unsigned long *arr, unsigned long len, int kind) {
    int matched = 1, skipped = 0;
    unsigned long posCond = 0;
    for (unsigned long i = 0; i + 1 < len; i += 2) {
        unsigned long w0 = arr[i], w1 = arr[i + 1];
        if ((w0 >> 31) & 1UL) {
            unsigned long br = (w0 >> 28) & 3UL;
            if (br == 3) { matched = 1; skipped = 0; }
            else if (br == 2) { matched = skipped ? 0 : 1; }
            else posCond = w0;
        } else if ((w0 >> 30) & 1UL) {
            if (!skipped && CheckPhyCond(posCond)) { matched = 1; skipped = 1; }
            else { matched = 0; if (!skipped) skipped = 0; }
        } else if (matched) {
            u16 addr = (u16)(w0 & 0xFFFF);
            if (kind == 0) RegWrite8(addr, (u8)(w1 & 0xFF));
            else if (kind == 1) {
                if (addr == 0xFE)      { BatchFlush(); Sleep(50); }
                else if (addr == 0xFD) { BatchFlush(); Sleep(5);  }
                else if (addr == 0xFC) { BatchFlush(); Sleep(1);  }
                else if (addr >= 0xF9 && addr <= 0xFB) BatchPush(RTW_OP_DELAY_READS, 1, 0, 0, 2);
                else RegWrite32(addr, (u32)w1);
            } else if (kind == 2) RegWrite32(addr, (u32)w1);
            else {
                if (addr == 0xFFE) { BatchFlush(); Sleep(50); }
                else if (addr == 0xFE) { BatchFlush(); Sleep(1); }
                else RfWrite((kind == 3) ? 0 : 1, addr, 0xFFFFFU, (u32)w1);
            }
        }
    }
    BatchFlush();
}

static void SetChRfeCca(u8 ch) {
    if (g_RfeOpt == 2) {
        RegWrite32(0x082C, 0x75C97010U); RegWrite32(0x0830, 0x79A0EAACU); RegWrite32(0x0838, 0x87746341U);
        RegMask32S(0x0CB0, 0xFFFFFFU, 0x705770U);
        RegMask32S(0x0CB4, 0xFF00U, 0x57U);
        RegMask32S(0x0CB8, (1U << 4), 0);
    } else {
        RegWrite32(0x082C, 0x75DA8010U); RegWrite32(0x0830, 0x97A0EAACU); RegWrite32(0x0838, 0x86666341U);
        RegMask32S(0x0CB0, 0xFFFFFFU, 0x745774U);
        RegMask32S(0x0CB4, 0xFF00U, 0x57U);
    }
    RegMask32S(0x0CBC, (1U << 11) | (1U << 10) | 0x3FU, 0);
    RegMask32S(0x0CA0, 0xFFFFU, 0xA501U);
    (void)ch;
}

static void ToggleIgi(void) {
    u32 igi = RegReadMask32(0x0C50, 0x7FU);
    if (igi < 2) igi = 0x1C;
    RegMask32(0x0C50, 0x7FU, igi - 2); RegMask32(0x0C50, 0x7FU, igi);
    RegMask32(0x0E50, 0x7FU, igi - 2); RegMask32(0x0E50, 0x7FU, igi);
    RegMask32(0x0808, 0xFFU, 0x00);    RegMask32(0x0808, 0xFFU, 0x33);
    BatchFlush();
}

static void RtwSetTxPower2G(void) {
    /* Program strong, clean 2.4 GHz TX AGC power indices (0x2A = 42/63)
     * across CCK, OFDM, HT, VHT rates on Path A (0x1D00) and Path B (0x1D80) */
    for (u16 off = 0; off < 0x50; off += 4) {
        RegWrite32((u16)(0x1D00 + off), 0x2A2A2A2AU);
        RegWrite32((u16)(0x1D80 + off), 0x2A2A2A2AU);
    }
    BatchFlush();
}

static void RtwPhySetParam(void) {
    RegSet8(0x0002, 0x03);
    RegSet8(0x001F, 0x07);
    RegSet32(0x00EC, 0x07000000U);
    RegClr32(0x0808, 0x30000000U);
    BatchFlush();

    LoadPhyTable(rtw8822b_mac,  RTW8822B_MAC_LEN,  0);
    LoadPhyTable(rtw8822b_bb,   RTW8822B_BB_LEN,   1);
    LoadPhyTable(rtw8822b_agc,  RTW8822B_AGC_LEN,  2);
    LoadPhyTable(rtw8822b_rf_a, RTW8822B_RF_A_LEN, 3);
    LoadPhyTable(rtw8822b_rf_b, RTW8822B_RF_B_LEN, 4);

    u32 xcap = g_XtalK & 0x3FU;
    RegMask32(0x0024, 0x7E000000U, xcap);
    RegMask32(0x0028, 0x0000007EU, xcap);
    RegSet32(0x0808, 0x30000000U);
    BatchFlush();

    /* TRX Mode 2T2R */
    RegMask32(0x0C08, 0xFFFFU, 0x3231); RegMask32(0x0E08, 0xFFFFU, 0x3231);
    RegMask32(0x093C, (3U << 18), 3);   RegMask32(0x080C, (3U << 28), 1);
    RegMask32(0x080C, (1U << 30), 1);   RegMask32(0x093C, 0xFFF00000U, 1);
    RegMask32(0x0A04, 0xF0000000U, 8);  RegMask32(0x0940, 0xFFF0U, 0x43);
    RegMask32(0x080C, 0xFFU, 0x33);     RegMask32(0x0A2C, (1U << 22), 0);
    RegMask32(0x0A2C, (1U << 18), 0);   RegMask32(0x0A04, 0x0F000000U, 0);
    RegMask32(0x0808, 0xFFU, 0x33);     RegMask32(0x1904, (1U << 16), 1);
    RegMask32(0x0800, (1U << 28), 1);   RegMask32(0x0850, (1U << 23), 1);
    BatchFlush();
    for (int i = 0; i < 50; i++) {
        RfWrite(0, 0xEF, 0xFFFFFU, 0x80000U); RfWrite(0, 0x33, 0xFFFFFU, 1);
        BatchFlush();
        if (RfRead(0, 0x33) == 1) break;
    }
    RfWrite(0, 0xEF, 0xFFFFFU, 0x80000U); RfWrite(0, 0x33, 0xFFFFFU, 1);
    RfWrite(0, 0x3E, 0xFFFFFU, 0x00034U); RfWrite(0, 0x3F, 0xFFFFFU, 0x4080CU);
    RfWrite(0, 0xEF, 0xFFFFFU, 0);        RfWrite(0, 0xEF, 0xFFFFFU, 0);
    BatchFlush();
    ToggleIgi();
    SetChRfeCca(1);

    /* RFE & DPDT -> Wi-Fi */
    RegMask32(0x0064, (3U << 28), 3); RegMask32(0x004C, (3U << 25), 0);
    RegMask32(0x0040, (1U << 2), 1);  RegMask32(0x1990, 0x3FU, 0x30);
    RegMask32(0x1990, (3U << 10), 3); RegMask32(0x0974, 0x3FU, 0x3F);
    RegMask32(0x0974, (3U << 10), 3);
    RegMask8(0x004E, 0x80, 0);        RegMask8(0x004F, 0x01, 1);

    /* Adaptivity & MAC address */
    RegMask32(0x08A4, 0xFFU, 0x7F); RegMask32(0x08A4, 0xFF00U, 0x7F);
    RegClr32(0x0520, (1U << 15));   RegSet32(0x0524, (1U << 11));
    RegMask32(0x0944, (3U << 28), 1); RegMask32(0x08A0, 3U, 0);
    RegSet32(0x08DC, (1U << 5));    RegWrite32(0x1C94, 0xAFFFAFFFU);

    for (int i = 0; i < 6; i++) RegWrite8((u16)(0x0610 + i), g_StaMac[i]);
    RegWrite32(0x0620, 0xFFFFFFFFU);
    RegWrite32(0x0624, 0xFFFFFFFFU);
    RegWrite16(0x06A0, 0xFFFF);
    RegWrite16(0x06A2, 0x0FFF);
    RegWrite16(0x06A4, 0xFFFF);
    RegWrite32(0x0608, 0xF400220FU);
    RegMask32(0x0C50, 0x7FU, 0x1C);
    RegMask32(0x0E50, 0x7FU, 0x1C);
    BatchFlush();

    RtwSetTxPower2G();
}

static void RtwSetChannel2G(u8 ch) {
    RegMask32(0x0808, (1U << 28), 1); RegMask32(0x0454, (1U << 7), 0);
    RegMask32(0x0A80, (1U << 18), 0); RegMask32(0x0814, 0xFC00U, 15);
    RegMask32(0x0958, 0x1FU, 0);      RegMask32(0x0860, 0x1FFE0000U, 0x96A);
    RegWrite32(0x0A24, (ch == 14) ? 0x00006577U : 0x384F6577U);
    RegMask32(0x0A28, 0xFFFFU, (ch == 14) ? 0 : 0x1525);
    RegMask32(0x0CBC, 0x0300U, 2);
    BatchFlush();
    RegWrite32(0x08AC, RegRead32(0x08AC) & 0xFFCFFC00U);
    RegMask32(0x08C4, (1U << 30), 1);

    RegWrite8(0x0483, 0);
    RegClr32(0x0668, (1U << 7) | (1U << 8));
    RegMask32(0x0024, (3U << 20), 0);
    RegWrite8(0x055C, 80); RegWrite8(0x0638, 80); RegClr8(0x0454, 0x80);
    BatchFlush();

    u32 rf18 = RfRead(0, 0x18);
    rf18 &= ~((1U<<16)|(1U<<9)|(1U<<8)|0xFFU|(1U<<18)|(1U<<17)|(1U<<11)|(1U<<10));
    rf18 |= (u32)ch | (1U << 11) | (1U << 10);
    RfWrite(0, 0xBE, (7U << 15), 0); RfWrite(0, 0xDF, (1U << 18), 0);
    RfWrite(0, 0x18, 0xFFFFFU, rf18); RfWrite(1, 0x18, 0xFFFFFU, rf18);
    RfWrite(0, 0xB8, (1U << 19), 0);  RfWrite(0, 0xB8, (1U << 19), 1);

    RegMask32(0x0948, (3U << 28), 2); RegMask32(0x094C, (3U << 28), 2);
    RegMask32S(0x0C20, (1U << 31), 1);
    BatchFlush();

    ToggleIgi();
    SetChRfeCca(ch);
    RegMask32(0x0C50, 0x7FU, 0x1C); RegMask32(0x0E50, 0x7FU, 0x1C);
    BatchFlush();
    RtwSetTxPower2G();
}

/* =========================================================================
 * 802.11 Frame Transmission via Bulk OUT (48-byte RTL8822B TX Descriptor)
 * ========================================================================= */
static int RtwSend80211Ex(HANDLE hOutDev, RTW_BULK_TX_PIPE *txBuf,
                          const u8 *frame, u32 frameLen,
                          u32 qsel, u32 rate, u32 rateId, int useHwSeq) {
    if (frameLen < 16 || frameLen > 2000) return 0;
    int isBmc = (frame[4] & 1) ? 1 : 0;
    u16 seq = (u16)((frame[22] | ((u16)frame[23] << 8)) >> 4);

    EnterCriticalSection(&g_TxLock);
    c_zero(txBuf->Data, 48);
    u32 *dw = (u32 *)txBuf->Data;

    dw[0] = (frameLen & 0xFFFFU) | (48U << 16) |
            (isBmc ? (1U << 24) : 0U) | (1U << 26) |
            (useHwSeq ? (1U << 31) : 0U); /* LS=1 | [DISQSELSEQ=1] */
    dw[1] = (qsel << 8) | (rateId << 16);
    dw[2] = 0;
    dw[3] = (1U << 8) | (1U << 10);       /* USE_RATE=1 | DISDATAFB=1 (no FW RA needed!) */
    dw[4] = rate;
    dw[5] = 0; /* BW20 */
    dw[6] = 0;
    dw[8] = useHwSeq ? (1U << 15) : 0U;   /* [EN_HWSEQ=1] */
    dw[9] = ((u32)(seq & 0xFFF) << 12);
    dw[7] = CalcTxDescChecksum(txBuf->Data);

    c_copy(txBuf->Data + 48, frame, frameLen);

    u32 totalLen = 48U + frameLen;
    /* Terminate USB Bulk OUT cleanly if transfer lands on a MaxPacketSize boundary */
    if ((totalLen & (g_BulkOutMaxPkt - 1)) == 0) {
        txBuf->Data[totalLen] = 0;
        totalLen += 1;
    }

    txBuf->OutPipeIdx = (qsel == 18U) ? g_MgmtPipeIdx : g_DataPipeIdx;
    txBuf->Length     = totalLen;

    DWORD br = 0;
    int ok = DeviceIoControl(hOutDev, IOCTL_RTW_BULK_TX_PIPE,
                             txBuf, 8 + totalLen, NULL, 0, &br, NULL) ? 1 : 0;
    LeaveCriticalSection(&g_TxLock);
    return ok;
}

static int RtwSend80211(HANDLE hOutDev, RTW_BULK_TX_PIPE *txBuf,
                        const u8 *frame, u32 frameLen, int isMgmt) {
    if (isMgmt) {
        return RtwSend80211Ex(hOutDev, txBuf, frame, frameLen, 18U, 0U, 8U, 1);
    }
    return RtwSend80211Ex(hOutDev, txBuf, frame, frameLen,
                          g_DataQsel, g_DataRate, g_DataRateId, g_DataHwSeq);
}

static u16 NextSeq(void) {
    u16 s;
    EnterCriticalSection(&g_Lock);
    s = (u16)(g_TxSeq++ & 0xFFF);
    LeaveCriticalSection(&g_Lock);
    return s;
}

/* =========================================================================
 * 802.11 Management Frames (Probe Req, Auth, Assoc Req, Null Keepalive)
 * ========================================================================= */
static void SendProbeReq(void) {
    u8 *f = g_FrameMain;
    c_zero(f, 128);
    f[0] = 0x40; f[1] = 0x00; /* Probe Request */
    for (int i = 0; i < 6; i++) { f[4 + i] = 0xFF; f[16 + i] = 0xFF; }
    c_copy(f + 10, g_StaMac, 6);
    u16 sc = (u16)(NextSeq() << 4);
    f[22] = (u8)sc; f[23] = (u8)(sc >> 8);
    u32 p = 24;
    /* SSID IE */
    f[p++] = 0; f[p++] = (u8)g_TargetSsidLen;
    c_copy(f + p, g_TargetSsid, g_TargetSsidLen); p += g_TargetSsidLen;
    /* Supported Rates (802.11b/g) */
    static const u8 rates[] = { 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
    c_copy(f + p, rates, sizeof(rates)); p += sizeof(rates);
    static const u8 erates[] = { 50, 4, 0x30, 0x48, 0x60, 0x6C };
    c_copy(f + p, erates, sizeof(erates)); p += sizeof(erates);
    RtwSend80211(g_hDev, &g_TxMain, f, p, 1);
}

static void SendAuthReq(void) {
    u8 *f = g_FrameMain;
    c_zero(f, 64);
    f[0] = 0xB0; f[1] = 0x00; /* Authentication */
    c_copy(f + 4,  g_Bssid, 6);
    c_copy(f + 10, g_StaMac, 6);
    c_copy(f + 16, g_Bssid, 6);
    u16 sc = (u16)(NextSeq() << 4);
    f[22] = (u8)sc; f[23] = (u8)(sc >> 8);
    /* Open System (0), Seq 1, Status 0 */
    f[24] = 0x00; f[25] = 0x00;
    f[26] = 0x01; f[27] = 0x00;
    f[28] = 0x00; f[29] = 0x00;
    RtwSend80211(g_hDev, &g_TxMain, f, 30, 1);
}

static void BuildRsnIe(void) {
    /* Standard 22-byte WPA2-PSK-CCMP RSN IE matching AP's Group Cipher */
    u8 *r = g_RsnIe;
    r[0] = 48; r[1] = 20;
    r[2] = 0x01; r[3] = 0x00; /* Version 1 */
    c_copy(r + 4, g_ApGroupCipher, 4);
    r[8] = 0x01; r[9] = 0x00; /* 1 Pairwise Cipher: CCMP */
    r[10] = 0x00; r[11] = 0x0F; r[12] = 0xAC; r[13] = 0x04;
    r[14] = 0x01; r[15] = 0x00; /* 1 AKM Suite: PSK */
    r[16] = 0x00; r[17] = 0x0F; r[18] = 0xAC; r[19] = 0x02;
    r[20] = 0x00; r[21] = 0x00; /* RSN Capabilities */
}

static void SendAssocReq(void) {
    u8 *f = g_FrameMain;
    c_zero(f, 160);
    f[0] = 0x00; f[1] = 0x00; /* Association Request */
    c_copy(f + 4,  g_Bssid, 6);
    c_copy(f + 10, g_StaMac, 6);
    c_copy(f + 16, g_Bssid, 6);
    u16 sc = (u16)(NextSeq() << 4);
    f[22] = (u8)sc; f[23] = (u8)(sc >> 8);
    /* Capability: ESS(0x0001) | ShortPreamble(0x0020) | ShortSlot(0x0400) | [Privacy(0x0010)] */
    u16 cap = g_ApIsWpa2 ? 0x0431 : 0x0421;
    f[24] = (u8)cap; f[25] = (u8)(cap >> 8);
    f[26] = 0x0A; f[27] = 0x00; /* Listen Interval = 10 */
    u32 p = 28;
    /* SSID IE */
    f[p++] = 0; f[p++] = (u8)g_TargetSsidLen;
    c_copy(f + p, g_TargetSsid, g_TargetSsidLen); p += g_TargetSsidLen;
    /* Supported + Extended Rates */
    static const u8 rates[]  = { 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
    static const u8 erates[] = { 50, 4, 0x30, 0x48, 0x60, 0x6C };
    c_copy(f + p, rates, sizeof(rates)); p += sizeof(rates);
    c_copy(f + p, erates, sizeof(erates)); p += sizeof(erates);
    if (g_ApIsWpa2) {
        BuildRsnIe();
        c_copy(f + p, g_RsnIe, 22);
        p += 22;
    }
    RtwSend80211(g_hDev, &g_TxMain, f, p, 1);
}

static void SendNullKeepalive(HANDLE hOutDev, RTW_BULK_TX_PIPE *txBuf) {
    u8 f[24];
    c_zero(f, 24);
    f[0] = 0x48; f[1] = 0x01; /* Null Data, ToDS=1, PwrMgt=0 (Active) */
    c_copy(f + 4,  g_Bssid, 6);
    c_copy(f + 10, g_StaMac, 6);
    c_copy(f + 16, g_Bssid, 6);
    u16 sc = (u16)(NextSeq() << 4);
    f[22] = (u8)sc; f[23] = (u8)(sc >> 8);
    RtwSend80211(hOutDev, txBuf, f, 24, 1);
}

/* =========================================================================
 * WPA2-PSK 4-Way Handshake & Group Key Handler (EAPOL 0x888E)
 * ========================================================================= */
static void GenSNonce(void) {
    u32 seed = (u32)GetTickCount() ^ 0xA5958822U;
    for (int i = 0; i < 6; i++) seed = (seed * 1664525U) ^ g_StaMac[i];
    for (int i = 0; i < 32; i++) {
        seed = seed * 1664525U + 1013904223U;
        g_SNonce[i] = (u8)((seed >> 16) & 0xFF);
    }
}

static void SendEapolFrame(HANDLE hOutDev, RTW_BULK_TX_PIPE *txBuf,
                           const u8 *eapol, u32 eapolLen, int encrypt) {
    u8 *f = g_FrameMain;
    u8 plain[256];
    if (eapolLen + 8 > sizeof(plain)) return;

    /* LLC/SNAP + EtherType 0x888E */
    plain[0] = 0xAA; plain[1] = 0xAA; plain[2] = 0x03;
    plain[3] = 0x00; plain[4] = 0x00; plain[5] = 0x00;
    plain[6] = 0x88; plain[7] = 0x8E;
    c_copy(plain + 8, eapol, eapolLen);

    c_zero(f, 24);
    f[0] = 0x08;
    f[1] = encrypt ? 0x41 : 0x01; /* Data, ToDS=1, [Protected] */
    c_copy(f + 4,  g_Bssid, 6);
    c_copy(f + 10, g_StaMac, 6);
    c_copy(f + 16, g_Bssid, 6);
    u16 sc = (u16)(NextSeq() << 4);
    f[22] = (u8)sc; f[23] = (u8)(sc >> 8);

    u32 bodyLen;
    if (encrypt) {
        u8 pn[6];
        EnterCriticalSection(&g_Lock);
        u32 lo = g_TxPnLo++, hi = g_TxPnHi;
        if (g_TxPnLo == 0) g_TxPnHi++;
        u8 tk[16];
        c_copy(tk, g_Ptk + 32, 16);
        LeaveCriticalSection(&g_Lock);
        pn[0]=(u8)lo; pn[1]=(u8)(lo>>8); pn[2]=(u8)(lo>>16); pn[3]=(u8)(lo>>24);
        pn[4]=(u8)hi; pn[5]=(u8)(hi>>8);
        bodyLen = ccmp_encrypt(tk, f, 24, 0, pn, plain, eapolLen + 8, f + 24);
    } else {
        bodyLen = eapolLen + 8;
        c_copy(f + 24, plain, bodyLen);
    }
    /* Send EAPOL at 1 Mbps (mgmt rate reliability!) */
    RtwSend80211(hOutDev, txBuf, f, 24 + bodyLen, 1);
}

static void ParseGtkKdes(const u8 *kd, u32 kdLen) {
    u32 pos = 0;
    while (pos + 2 <= kdLen) {
        u8 id = kd[pos];
        u8 el = kd[pos + 1];
        if (id == 0xDD && el == 0) break; /* Padding */
        if (pos + 2 + el > kdLen) break;
        const u8 *ed = kd + pos + 2;
        if (id == 0xDD && el >= 22 && ed[0] == 0x00 && ed[1] == 0x0F && ed[2] == 0xAC && ed[3] == 0x01) {
            u8 kid = (u8)(ed[4] & 0x03);
            EnterCriticalSection(&g_Lock);
            c_copy(g_Gtk[kid], ed + 6, 16);
            g_GtkValid[kid] = 1;
            LeaveCriticalSection(&g_Lock);
            Print("  [WPA2] Installed GTK keyId="); PrintDec(kid); Print("\r\n");
        }
        pos += 2 + el;
    }
}

static void HandleEapol(const u8 *eapol, u32 len, int rxWasEncrypted) {
    if (len < 99) return;
    if (eapol[1] != 0x03) return; /* EAPOL-Key */
    u16 bodyLen = (u16)(((u16)eapol[2] << 8) | eapol[3]);
    if ((u32)bodyLen + 4 > len || bodyLen < 95) return;
    u32 totalLen = (u32)bodyLen + 4;
    if (eapol[4] != 0x02) return; /* RSN Key Descriptor */

    u16 keyInfo = (u16)(((u16)eapol[5] << 8) | eapol[6]);
    u16 kdLen   = (u16)(((u16)eapol[97] << 8) | eapol[98]);
    const u8 *replay = eapol + 9;
    const u8 *anonce = eapol + 17;
    const u8 *rxMic  = eapol + 81;
    const u8 *kd     = eapol + 99;

    int pairwise = (keyInfo & 0x0008) ? 1 : 0;
    int install  = (keyInfo & 0x0040) ? 1 : 0;
    int ack      = (keyInfo & 0x0080) ? 1 : 0;
    int micBit   = (keyInfo & 0x0100) ? 1 : 0;
    int secure   = (keyInfo & 0x0200) ? 1 : 0;
    int encKd    = (keyInfo & 0x1000) ? 1 : 0;

    g_StatEapol++;

    if (pairwise && ack && !micBit) {
        /* ===== MESSAGE 1/4 ===== */
        Print("  [WPA2] RX M1/4 -> deriving PTK & sending M2/4...\r\n");
        c_copy(g_ANonce, anonce, 32);
        if (g_HandshakeState == 0) GenSNonce();
        wpa2_derive_ptk(g_Pmk, g_Bssid, g_StaMac, g_ANonce, g_SNonce, g_Ptk);

        /* Build M2/4 (99 + 22 = 121 bytes) */
        u8 m2[160];
        c_zero(m2, sizeof(m2));
        u16 m2Body = 95 + 22;
        m2[0] = 0x01; m2[1] = 0x03;
        m2[2] = (u8)(m2Body >> 8); m2[3] = (u8)m2Body;
        m2[4] = 0x02; /* RSN */
        m2[5] = 0x01; m2[6] = 0x0A; /* Pairwise | MIC | Ver 2 */
        c_copy(m2 + 9, replay, 8);
        c_copy(m2 + 17, g_SNonce, 32);
        m2[97] = 0x00; m2[98] = 22;
        c_copy(m2 + 99, g_RsnIe, 22);

        u8 hmacOut[20];
        hmac_sha1(g_Ptk, 16, m2, 4 + m2Body, hmacOut);
        c_copy(m2 + 81, hmacOut, 16);

        SendEapolFrame(g_hDev, &g_TxMain, m2, 4 + m2Body, 0);
        g_HandshakeState = 1;
    } else if (pairwise && ack && micBit && install) {
        /* ===== MESSAGE 3/4 ===== */
        /* Verify MIC */
        u8 tmp[512];
        if (totalLen > sizeof(tmp)) return;
        c_copy(tmp, eapol, totalLen);
        c_zero(tmp + 81, 16);
        u8 calc[20];
        hmac_sha1(g_Ptk, 16, tmp, totalLen, calc);
        if (c_cmp(calc, rxMic, 16) != 0) {
            Print("  [WPA2] M3/4 MIC MISMATCH (wrong password?)\r\n");
            return;
        }
        Print("  [WPA2] RX M3/4 (MIC OK!) -> unwrapping GTK & sending M4/4...\r\n");

        if (encKd && kdLen >= 16 && (u32)kdLen + 99 <= totalLen && kdLen <= 256) {
            u8 unw[256];
            if (aes128_key_unwrap(g_Ptk + 16, kd, kdLen, unw)) {
                ParseGtkKdes(unw, kdLen - 8);
            } else {
                Print("  [WPA2] M3/4 GTK unwrap failed!\r\n");
            }
        }

        /* Build M4/4 (99 bytes) */
        u8 m4[104];
        c_zero(m4, sizeof(m4));
        m4[0] = 0x01; m4[1] = 0x03;
        m4[2] = 0x00; m4[3] = 95;
        m4[4] = 0x02;
        m4[5] = 0x03; m4[6] = 0x0A; /* Pairwise | MIC | Secure | Ver 2 */
        c_copy(m4 + 9, replay, 8);
        u8 hmacOut[20];
        hmac_sha1(g_Ptk, 16, m4, 99, hmacOut);
        c_copy(m4 + 81, hmacOut, 16);

        /* Send M4 unencrypted (or encrypted if rekeying while already encrypted) */
        SendEapolFrame(g_hDev, &g_TxMain, m4, 99, rxWasEncrypted);

        if (!g_Encrypted) {
            EnterCriticalSection(&g_Lock);
            g_TxPnLo = 1; g_TxPnHi = 0;
            g_Encrypted = 1;
            g_HandshakeState = 2;
            LeaveCriticalSection(&g_Lock);
            Print("  [WPA2] 4-WAY HANDSHAKE COMPLETE! AES-CCMP ACTIVE!\r\n");
        }
    } else if (!pairwise && ack && micBit && secure) {
        /* ===== GROUP KEY REKEY (GTK M1/2) ===== */
        u8 tmp[512];
        if (totalLen > sizeof(tmp)) return;
        c_copy(tmp, eapol, totalLen);
        c_zero(tmp + 81, 16);
        u8 calc[20];
        hmac_sha1(g_Ptk, 16, tmp, totalLen, calc);
        if (c_cmp(calc, rxMic, 16) != 0) return;
        if (encKd && kdLen >= 16 && (u32)kdLen + 99 <= totalLen && kdLen <= 256) {
            u8 unw[256];
            if (aes128_key_unwrap(g_Ptk + 16, kd, kdLen, unw)) {
                ParseGtkKdes(unw, kdLen - 8);
            }
        }
        u8 gm2[104];
        c_zero(gm2, sizeof(gm2));
        gm2[0] = 0x01; gm2[1] = 0x03;
        gm2[2] = 0x00; gm2[3] = 95;
        gm2[4] = 0x02;
        gm2[5] = 0x03; gm2[6] = 0x02; /* Group | MIC | Secure | Ver 2 */
        c_copy(gm2 + 9, replay, 8);
        u8 hmacOut[20];
        hmac_sha1(g_Ptk, 16, gm2, 99, hmacOut);
        c_copy(gm2 + 81, hmacOut, 16);
        SendEapolFrame(g_hDev, &g_TxMain, gm2, 99, 1);
    }
}

/* =========================================================================
 * Traffic Logging Helper (ARP / DHCP / ICMP / DNS)
 * ========================================================================= */
static void LogEthSummary(const char *dir, const u8 *eth, u32 len) {
    if (len < 14) return;
    u16 et = (u16)(((u16)eth[12] << 8) | eth[13]);
    if (et == 0x0806 && len >= 42) {
        u16 op = (u16)(((u16)eth[20] << 8) | eth[21]);
        Print(dir); Print(" ARP "); Print((op == 1) ? "REQ " : "REP ");
        PrintIp(eth + 28); Print(" -> "); PrintIp(eth + 38); Print("\r\n");
    } else if (et == 0x0800 && len >= 34) {
        u8 proto = eth[23];
        if (proto == 1) {
            u8 itype = eth[34];
            Print(dir); Print(" ICMP ");
            if (itype == 8) Print("PING ");
            else if (itype == 0) Print("PONG ");
            else { Print("t="); PrintDec(itype); Print(" "); }
            PrintIp(eth + 26); Print(" -> "); PrintIp(eth + 30); Print("\r\n");
        } else if (proto == 17 && len >= 42) {
            u16 sp = (u16)(((u16)eth[34] << 8) | eth[35]);
            u16 dp = (u16)(((u16)eth[36] << 8) | eth[37]);
            if (sp == 67 || sp == 68 || dp == 67 || dp == 68) {
                Print(dir); Print(" DHCP ");
                PrintIp(eth + 26); Print(" -> "); PrintIp(eth + 30);
                Print(" ("); PrintDec(len); Print("B)\r\n");
            } else if (sp == 53 || dp == 53) {
                Print(dir); Print(" DNS ");
                PrintIp(eth + 26); Print(" -> "); PrintIp(eth + 30); Print("\r\n");
            }
        }
    }
}

static int BuildAndSendEth80211(HANDLE hOutDev, RTW_BULK_TX_PIPE *txBuf,
                                u8 *frameBuf, u8 *plainBuf,
                                const u8 *eth, u32 ethLen,
                                u32 qsel, u32 rate, u32 rateId, int useHwSeq) {
    if (ethLen < 14 || ethLen > 1518) return 0;

    /* Build 802.11 Data frame (ToDS=1) */
    c_zero(frameBuf, 24);
    frameBuf[0] = 0x08;
    frameBuf[1] = g_Encrypted ? 0x41 : 0x01;
    c_copy(frameBuf + 4,  g_Bssid, 6);
    c_copy(frameBuf + 10, g_StaMac, 6);
    c_copy(frameBuf + 16, eth, 6); /* DA = Ethernet Dst MAC */
    u16 sc = (u16)(NextSeq() << 4);
    frameBuf[22] = (u8)sc; frameBuf[23] = (u8)(sc >> 8);

    /* LLC/SNAP + payload */
    plainBuf[0] = 0xAA; plainBuf[1] = 0xAA; plainBuf[2] = 0x03;
    plainBuf[3] = 0x00; plainBuf[4] = 0x00; plainBuf[5] = 0x00;
    plainBuf[6] = eth[12];
    plainBuf[7] = eth[13];
    u32 payLen = ethLen - 14;
    c_copy(plainBuf + 8, eth + 14, payLen);
    u32 plainLen = payLen + 8;

    u32 bodyLen;
    if (g_Encrypted) {
        u8 pn[6], tk[16];
        EnterCriticalSection(&g_Lock);
        u32 lo = g_TxPnLo++, hi = g_TxPnHi;
        if (g_TxPnLo == 0) g_TxPnHi++;
        c_copy(tk, g_Ptk + 32, 16);
        LeaveCriticalSection(&g_Lock);
        pn[0]=(u8)lo; pn[1]=(u8)(lo>>8); pn[2]=(u8)(lo>>16); pn[3]=(u8)(lo>>24);
        pn[4]=(u8)hi; pn[5]=(u8)(hi>>8);
        bodyLen = ccmp_encrypt(tk, frameBuf, 24, 0, pn, plainBuf, plainLen, frameBuf + 24);
    } else {
        bodyLen = plainLen;
        c_copy(frameBuf + 24, plainBuf, plainLen);
    }

    return RtwSend80211Ex(hOutDev, txBuf, frameBuf, 24 + bodyLen, qsel, rate, rateId, useHwSeq);
}

/* =========================================================================
 * Single-Threaded TX Drainer: RTW88.VXD (DIOC 2) -> 802.11 -> Bulk OUT
 * Uses g_hDev sequentially (zero concurrent USB Bulk IN/OUT on Win95 OHCI!)
 * ========================================================================= */
static void FlushStaleVxdTx(void) {
    if (g_hVxd == INVALID_HANDLE_VALUE) return;
    int dropped = 0;
    for (int i = 0; i < 64; i++) {
        DWORD txLen = 0;
        if (!DeviceIoControl(g_hVxd, RTW88_DIOC_POP_TX,
                             NULL, 0, g_EthTxBuf, sizeof(g_EthTxBuf), &txLen, NULL) || txLen == 0) {
            break;
        }
        dropped++;
    }
    if (dropped > 0) {
        Print("  Flushed "); PrintDec((u32)dropped); Print(" stale queued TX frame(s) from RTW88.VXD\r\n");
    }
}

static int DrainVxdTx(void) {
    if (g_hVxd == INVALID_HANDLE_VALUE) return 0;
    if (g_ApIsWpa2 && !g_Encrypted) return 0;

    int sent = 0;
    for (int burst = 0; burst < 16; burst++) {
        DWORD txLen = 0;
        BOOL popped = DeviceIoControl(g_hVxd, RTW88_DIOC_POP_TX,
                                      NULL, 0, g_EthTxBuf, sizeof(g_EthTxBuf), &txLen, NULL);
        if (!popped || txLen < 14 || txLen > 1518) break;

        if (BuildAndSendEth80211(g_hDev, &g_TxMain, g_FrameMain, g_PlainMain,
                                 g_EthTxBuf, txLen,
                                 g_DataQsel, g_DataRate, g_DataRateId, g_DataHwSeq)) {
            g_StatTxEth++;
            sent++;
            if (g_StatTxEth <= 5) {
                Print("[TX#"); PrintDec(g_StatTxEth); Print("] len="); PrintDec(txLen);
                Print(" src="); PrintMac(g_EthTxBuf + 6);
                Print(" dst="); PrintMac(g_EthTxBuf);
                Print(" et="); PrintHex2(g_EthTxBuf[12]); PrintHex2(g_EthTxBuf[13]);
                Print("\r\n");
            }
            LogEthSummary("[TX]", g_EthTxBuf, txLen);
        } else {
            Print("[TX USB FAIL] err="); PrintDec(GetLastError()); Print("\r\n");
            break;
        }
    }
    return sent;
}

/* =========================================================================
 * RX Bulk IN Packet Reader & 802.11 Frame Dispatcher
 * ========================================================================= */
typedef void (*FRAME_CB)(const u8 *frame, u32 len, void *ctx);

static int PollRxBatch(FRAME_CB cb, void *ctx) {
    RTW_BULK_RX_REQ req;
    req.CheckFifo = 0;
    req.MaxLen    = 4096;
    DWORD br = 0;
    if (!DeviceIoControl(g_hDev, IOCTL_RTW_BULK_RX_RAW,
                         &req, sizeof(req), &g_RxRaw, sizeof(g_RxRaw), &br, NULL)) {
        return -1;
    }
    if (g_RxRaw.Status < 0) return -1;
    if (g_RxRaw.Length < 24) return 0;

    u32 off = 0;
    int cnt = 0;
    while (off + 24 <= g_RxRaw.Length) {
        const u8 *p = g_RxRaw.Data + off;
        u32 w0 = *(const u32 *)(p + 0);
        u32 w2 = *(const u32 *)(p + 8);
        u32 pktLen = w0 & 0x3FFFU;
        u32 crcErr = (w0 >> 14) & 1U;
        u32 drvSz  = ((w0 >> 16) & 0xFU) * 8U;
        u32 shift  = (w0 >> 24) & 3U;
        u32 isC2h  = (w2 >> 28) & 1U;
        u32 pktOff = 24U + shift + drvSz;

        if (pktLen == 0 || pktOff + pktLen > g_RxRaw.Length - off) break;
        if (!isC2h && !crcErr && pktLen > 4) {
            /* Strip 4-byte 802.11 FCS (BIT_APP_FCS is enabled in RCR) */
            cb(p + pktOff, pktLen - 4, ctx);
            cnt++;
        }
        u32 step = (pktOff + pktLen + 7U) & ~7U;
        if (step == 0) break;
        off += step;
    }
    return cnt;
}

/* Scan callback: find target SSID */
typedef struct { u8 curCh; int found; } SCAN_CTX;
static void ScanFrameCb(const u8 *f, u32 len, void *vctx) {
    SCAN_CTX *ctx = (SCAN_CTX *)vctx;
    if (ctx->found || len < 36) return;
    if (f[0] != 0x80 && f[0] != 0x50) return; /* Beacon or Probe Resp */

    const u8 *bssid = f + 16;
    u16 cap = (u16)(f[34] | ((u16)f[35] << 8));
    int isWpa2 = 0;
    u8 ch = ctx->curCh;
    u8 grp[4] = { 0x00, 0x0F, 0xAC, 0x04 };
    int ssidMatch = 0;

    u32 pos = 36;
    while (pos + 2 <= len) {
        u8 id = f[pos], el = f[pos + 1];
        if (pos + 2 + el > len) break;
        const u8 *ed = f + pos + 2;
        if (id == 0 && el == g_TargetSsidLen && c_cmp(ed, g_TargetSsid, el) == 0) {
            ssidMatch = 1;
        } else if (id == 3 && el == 1 && ed[0] >= 1 && ed[0] <= 14) {
            ch = ed[0];
        } else if (id == 48 && el >= 6) {
            isWpa2 = 1;
            c_copy(grp, ed + 2, 4);
        }
        pos += 2 + el;
    }
    if (ssidMatch) {
        c_copy(g_Bssid, bssid, 6);
        g_ApChannel = ch;
        g_ApIsWpa2  = isWpa2 ? 1 : ((cap & 0x0010) ? 1 : 0);
        c_copy(g_ApGroupCipher, grp, 4);
        ctx->found = 1;
    }
}

/* Auth/Assoc callback */
typedef struct { int authOk; int assocOk; u16 status; } CONN_CTX;
static void ConnFrameCb(const u8 *f, u32 len, void *vctx) {
    CONN_CTX *ctx = (CONN_CTX *)vctx;
    if (len < 24) return;
    if (c_cmp(f + 4, g_StaMac, 6) != 0) return;
    if (c_cmp(f + 10, g_Bssid, 6) != 0) return;

    if (f[0] == 0xB0 && len >= 30) {
        u16 seq = (u16)(f[26] | ((u16)f[27] << 8));
        u16 st  = (u16)(f[28] | ((u16)f[29] << 8));
        if (seq == 2) {
            ctx->status = st;
            if (st == 0) ctx->authOk = 1;
        }
    } else if (f[0] == 0x10 && len >= 30) {
        u16 st  = (u16)(f[26] | ((u16)f[27] << 8));
        u16 aid = (u16)((f[28] | ((u16)f[29] << 8)) & 0x3FFF);
        ctx->status = st;
        if (st == 0) {
            g_Aid = aid;
            ctx->assocOk = 1;
        }
    }
}

static void DeliverLlcToVxd(const u8 *da, const u8 *sa, const u8 *llc, u32 llcLen,
                            int wasEnc, int isUcast) {
    if (llcLen < 8) return;
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03 ||
        llc[3] != 0x00 || llc[4] != 0x00 || llc[5] != 0x00) {
        if (isUcast) {
            Print("[RX UCAST NON-LLC] len="); PrintDec(llcLen); Print(" b=");
            for (u32 i = 0; i < 8 && i < llcLen; i++) { PrintHex2(llc[i]); Print(" "); }
            Print("\r\n");
        }
        return;
    }

    u16 ethType = (u16)(((u16)llc[6] << 8) | llc[7]);
    if (ethType == 0x888E) {
        HandleEapol(llc + 8, llcLen - 8, wasEnc);
        return;
    }

    if (g_ApIsWpa2 && !g_Encrypted) return;

    /* Reassemble 802.3 Ethernet frame & inject into RTW88.VXD (DIOC 3) */
    u32 payLen = llcLen - 8;
    if (payLen > 1500) return;
    u32 ethLen = 14 + payLen;
    c_copy(g_EthRxBuf + 0, da, 6);
    c_copy(g_EthRxBuf + 6, sa, 6);
    g_EthRxBuf[12] = llc[6];
    g_EthRxBuf[13] = llc[7];
    c_copy(g_EthRxBuf + 14, llc + 8, payLen);
    if (ethLen < 60) {
        c_zero(g_EthRxBuf + ethLen, 60 - ethLen);
        ethLen = 60;
    }

    if (g_hVxd != INVALID_HANDLE_VALUE) {
        DWORD br = 0;
        EnterCriticalSection(&g_VxdLock);
        DeviceIoControl(g_hVxd, RTW88_DIOC_INJECT_RX, g_EthRxBuf, ethLen, NULL, 0, &br, NULL);
        LeaveCriticalSection(&g_VxdLock);
    }
    g_StatRxEth++;
    LogEthSummary("[RX]", g_EthRxBuf, ethLen);
}

/* Connected Bridge RX callback */
static void BridgeRxCb(const u8 *f, u32 len, void *vctx) {
    (void)vctx;
    if (len < 24) return;
    u8 fc0 = f[0], fc1 = f[1];

    if (fc0 == 0x80 && c_cmp(f + 16, g_Bssid, 6) == 0) {
        g_StatRxBcn++;
        return;
    }

    if ((fc0 == 0xC0 || fc0 == 0xA0) && c_cmp(f + 4, g_StaMac, 6) == 0 && c_cmp(f + 10, g_Bssid, 6) == 0) {
        u16 reason = (len >= 26) ? (u16)(f[24] | ((u16)f[25] << 8)) : 0;
        Print("\r\n[WARN] AP sent Deauth/Disassoc (reason="); PrintDec(reason); Print(")!\r\n");
        return;
    }

    /* Data frame: Type=2 (0x08), not Null (bit 6 of fc0 == 0), FromDS=1 ToDS=0 */
    if ((fc0 & 0x0C) != 0x08) return;
    if (fc0 & 0x40) return; /* Null / QoS-Null */
    if ((fc1 & 0x03) != 0x02) return; /* Must be FromDS=1, ToDS=0 */

    const u8 *da    = f + 4;
    const u8 *bssid = f + 10;
    const u8 *sa    = f + 16;

    if (c_cmp(bssid, g_Bssid, 6) != 0) return;
    int isMcast = (da[0] & 1) ? 1 : 0;
    if (!isMcast && c_cmp(da, g_StaMac, 6) != 0) return;
    int isOwnEcho = (c_cmp(sa, g_StaMac, 6) == 0) ? 1 : 0;

    int isQos = ((fc0 & 0x80) == 0x80) ? 1 : 0;
    u32 hdrLen = isQos ? 26U : 24U;
    if (isQos && (fc1 & 0x80)) hdrLen += 4U; /* +HTC / Order bit */
    if (len <= hdrLen) return;
    int isAmsdu = (isQos && (f[24] & 0x80)) ? 1 : 0;

    const u8 *body = f + hdrLen;
    u32 bodyLen = len - hdrLen;

    const u8 *llc = body;
    u32 llcLen = bodyLen;
    int wasEnc = (fc1 & 0x40) ? 1 : 0;

    if (wasEnc) {
        if (bodyLen < 16) return;
        u8 keyId = (u8)((body[3] >> 6) & 3);
        u8 key[16];
        int haveKey = 0;
        EnterCriticalSection(&g_Lock);
        if (isMcast) {
            if (g_GtkValid[keyId]) { c_copy(key, g_Gtk[keyId], 16); haveKey = 1; }
        } else {
            if (g_HandshakeState >= 1) { c_copy(key, g_Ptk + 32, 16); haveKey = 1; }
        }
        LeaveCriticalSection(&g_Lock);
        if (!haveKey) return;
        if (!ccmp_decrypt(key, f, hdrLen, body, bodyLen, g_DecBuf, &llcLen)) {
            g_StatDecFail++;
            if (!isMcast) {
                Print("[RX UCAST DECFAIL] fc="); PrintHex2(fc0); Print(" "); PrintHex2(fc1);
                Print(" len="); PrintDec(len); Print(" hdr="); PrintDec(hdrLen);
                Print(" kid="); PrintDec(keyId); Print("\r\n");
            }
            return;
        }
        llc = g_DecBuf;
    }

    if (isOwnEcho) {
        g_StatTxEcho++;
        if (g_StatTxEcho <= 10) {
            Print("[ECHO OK!] AP received & decrypted our broadcast (#");
            PrintDec(g_StatTxEcho); Print(")\r\n");
        }
        return; /* Never loop our own broadcast back into RTW88.VXD */
    }

    if (!isMcast) g_StatRxUcast++;

    if (isAmsdu) {
        u32 off = 0;
        while (off + 14 <= llcLen) {
            const u8 *subDa = llc + off;
            const u8 *subSa = llc + off + 6;
            u32 subLen = ((u32)llc[off + 12] << 8) | llc[off + 13];
            if (off + 14 + subLen > llcLen) break;
            if (c_cmp(subSa, g_StaMac, 6) != 0) {
                DeliverLlcToVxd(subDa, subSa, llc + off + 14, subLen, wasEnc, !isMcast);
            }
            u32 step = (14U + subLen + 3U) & ~3U;
            if (step == 0) break;
            off += step;
        }
    } else {
        DeliverLlcToVxd(da, sa, llc, llcLen, wasEnc, !isMcast);
    }
}

/* Built-in live TX mode auto-probe: sends encrypted ARP Req to 192.168.1.1 and
 * checks for [ECHO OK!] (AP re-broadcast) or [RX] ARP REP to lock the best TX mode! */
static void RunTxSelfTest(void) {
    static const struct { u32 qsel; u32 rate; u32 rateId; int hwSeq; const char *name; } modes[] = {
        { 0U,  0U, 8U, 0, "QSEL=0(TID0) 1M-CCK SwSeq" },
        { 18U, 0U, 8U, 1, "QSEL=18(MGMT) 1M-CCK HwSeq" },
        { 0U,  4U, 6U, 0, "QSEL=0(TID0) 6M-OFDM SwSeq" },
        { 18U, 4U, 6U, 1, "QSEL=18(MGMT) 6M-OFDM HwSeq" }
    };

    /* Standard 42-byte Ethernet ARP Request: who-has 192.168.1.1 tell 192.168.1.150 */
    u8 arp[42];
    c_zero(arp, sizeof(arp));
    for (int i = 0; i < 6; i++) arp[i] = 0xFF;
    c_copy(arp + 6, g_StaMac, 6);
    arp[12] = 0x08; arp[13] = 0x06; /* ARP */
    arp[14] = 0x00; arp[15] = 0x01; /* Ethernet */
    arp[16] = 0x08; arp[17] = 0x00; /* IPv4 */
    arp[18] = 6; arp[19] = 4;
    arp[20] = 0x00; arp[21] = 0x01; /* Request */
    c_copy(arp + 22, g_StaMac, 6);
    arp[28] = 192; arp[29] = 168; arp[30] = 1; arp[31] = 150;
    arp[38] = 192; arp[39] = 168; arp[40] = 1; arp[41] = 1;

    Print("[TEST] Probing best Data TX mode (encrypted ARP -> 192.168.1.1)...\r\n");
    int locked = 0;
    for (int m = 0; m < 4 && !locked; m++) {
        u32 echo0 = g_StatTxEcho;
        u32 ucast0 = g_StatRxUcast;
        Print("  Try "); Print(modes[m].name); Print("... ");
        BuildAndSendEth80211(g_hDev, &g_TxMain, g_FrameMain, g_PlainMain,
                             arp, sizeof(arp),
                             modes[m].qsel, modes[m].rate, modes[m].rateId, modes[m].hwSeq);
        DWORD t0 = GetTickCount();
        while ((GetTickCount() - t0) < 600) {
            PollRxBatch(BridgeRxCb, NULL);
            if (g_StatTxEcho > echo0 || g_StatRxUcast > ucast0) {
                g_DataQsel   = modes[m].qsel;
                g_DataRate   = modes[m].rate;
                g_DataRateId = modes[m].rateId;
                g_DataHwSeq  = modes[m].hwSeq;
                Print("LOCKED! (Echo="); PrintDec(g_StatTxEcho - echo0);
                Print(" Ucast="); PrintDec(g_StatRxUcast - ucast0); Print(")\r\n");
                locked = 1;
                break;
            }
        }
        if (!locked) Print("no reply\r\n");
    }
    if (!locked) {
        /* Fallback to proven QSEL=0 1M-CCK SwSeq mode */
        g_DataQsel = 0U; g_DataRate = 0U; g_DataRateId = 8U; g_DataHwSeq = 0;
        Print("  [INFO] Using default QSEL=0 1M-CCK SwSeq.\r\n");
    } else if (g_DataRate == 0U && g_DataQsel == 0U) {
        /* Bonus check: see if AP also accepts 6 Mbps OFDM on QSEL=0 for 6x faster throughput */
        u32 echo0 = g_StatTxEcho, ucast0 = g_StatRxUcast;
        Print("  Try speed upgrade QSEL=0 6M-OFDM... ");
        BuildAndSendEth80211(g_hDev, &g_TxMain, g_FrameMain, g_PlainMain,
                             arp, sizeof(arp), 0U, 4U, 6U, 0);
        DWORD t0 = GetTickCount();
        int fastOk = 0;
        while ((GetTickCount() - t0) < 350) {
            PollRxBatch(BridgeRxCb, NULL);
            if (g_StatTxEcho > echo0 || g_StatRxUcast > ucast0) {
                g_DataRate = 4U; g_DataRateId = 6U;
                Print("UPGRADED to 6 Mbps OFDM!\r\n");
                fastOk = 1;
                break;
            }
        }
        if (!fastOk) Print("keeping 1 Mbps CCK\r\n");
    }
}

/* =========================================================================
 * Config Parser (CLI args or WIFI.TXT)
 * ========================================================================= */
static const char *SkipWs(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static const char *ParseArg(const char *p, char *out, u32 maxLen) {
    p = SkipWs(p);
    if (!*p) { out[0] = '\0'; return p; }
    u32 i = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i + 1 < maxLen) out[i++] = *p++;
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && i + 1 < maxLen) out[i++] = *p++;
    }
    out[i] = '\0';
    return p;
}

static u8 ParseDecU8(const char *s) {
    u32 v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (u32)(*s - '0'); s++; }
    return (v >= 1 && v <= 14) ? (u8)v : 0;
}

static int StrEqI(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return (*a == '\0' && *b == '\0');
}

static int LoadConfig(void) {
    const char *cmd = GetCommandLineA();
    char prog[128], a1[68], a2[68], a3[16];
    a1[0] = a2[0] = a3[0] = '\0';
    cmd = ParseArg(cmd, prog, sizeof(prog));
    cmd = ParseArg(cmd, a1, sizeof(a1));
    if (StrEqI(a1, "/tray") || StrEqI(a1, "-tray")) {
        g_StartInTray = 1;
        a1[0] = '\0';
        cmd = ParseArg(cmd, a1, sizeof(a1));
    }
    cmd = ParseArg(cmd, a2, sizeof(a2));
    cmd = ParseArg(cmd, a3, sizeof(a3));

    if (a1[0] != '\0') {
        g_TargetSsid[0] = '\0'; StrAppend(g_TargetSsid, sizeof(g_TargetSsid), a1);
        g_Password[0] = '\0';   StrAppend(g_Password, sizeof(g_Password), a2);
        g_ForceCh = ParseDecU8(a3);
        g_CliConnect = 1;
    }

    if (g_TargetSsid[0] == '\0') {
        HANDLE h = OpenLocalFileRead("WIFI.TXT");
        if (h != INVALID_HANDLE_VALUE) {
            char buf[320];
            DWORD br = 0;
            ReadFile(h, buf, sizeof(buf) - 1, &br, NULL);
            CloseHandle(h);
            buf[br] = '\0';

            /* Line 1 = SSID, Line 2 = Password, Line 3 = Channel (optional), Line 4 = AUTO=1/0 */
            u32 p = 0, i = 0;
            while (buf[p] && buf[p] != '\r' && buf[p] != '\n' && i + 1 < sizeof(g_TargetSsid)) {
                g_TargetSsid[i++] = buf[p++];
            }
            g_TargetSsid[i] = '\0';
            if (buf[p] == '\r') p++;
            if (buf[p] == '\n') p++;

            i = 0;
            while (buf[p] && buf[p] != '\r' && buf[p] != '\n' && i + 1 < sizeof(g_Password)) {
                g_Password[i++] = buf[p++];
            }
            g_Password[i] = '\0';
            if (buf[p] == '\r') p++;
            if (buf[p] == '\n') p++;

            char chStr[16];
            i = 0;
            while (buf[p] && buf[p] != '\r' && buf[p] != '\n' && i + 1 < sizeof(chStr)) {
                chStr[i++] = buf[p++];
            }
            chStr[i] = '\0';
            g_ForceCh = ParseDecU8(chStr);
            if (buf[p] == '\r') p++;
            if (buf[p] == '\n') p++;

            char autoStr[16];
            i = 0;
            while (buf[p] && buf[p] != '\r' && buf[p] != '\n' && i + 1 < sizeof(autoStr)) {
                autoStr[i++] = buf[p++];
            }
            autoStr[i] = '\0';
            if (StrEqI(autoStr, "AUTO=1") || StrEqI(autoStr, "1")) g_AutoConnect = 1;
        }
    }

    g_TargetSsidLen = StrLen(g_TargetSsid);
    g_PasswordLen   = StrLen(g_Password);
    return (g_TargetSsidLen > 0) ? 1 : 0;
}

static void SaveWifiConfig(void) {
    char path[300];
    BuildExeDirPath(path, sizeof(path), "WIFI.TXT");
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileA("WIFI.TXT", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) return;

    char buf[256];
    buf[0] = '\0';
    StrAppend(buf, sizeof(buf), g_TargetSsid);
    StrAppend(buf, sizeof(buf), "\r\n");
    StrAppend(buf, sizeof(buf), g_Password);
    StrAppend(buf, sizeof(buf), "\r\n");
    if (g_ForceCh >= 1 && g_ForceCh <= 14) {
        StrAppendDec(buf, sizeof(buf), g_ForceCh);
    }
    StrAppend(buf, sizeof(buf), "\r\n");
    StrAppend(buf, sizeof(buf), g_AutoConnect ? "AUTO=1\r\n" : "AUTO=0\r\n");
    DWORD w = 0;
    WriteFile(h, buf, StrLen(buf), &w, NULL);
    CloseHandle(h);
}

/* =========================================================================
 * Windows 95 Registry Auto-Start (Run in System Tray at Boot)
 * ========================================================================= */
static const char g_RunKeyPath[] = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char g_RunValName[] = "RTW88WiFi";

static int IsBootAutoStartEnabled(void) {
    HKEY hk = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, g_RunKeyPath, 0, KEY_READ, &hk) != ERROR_SUCCESS) {
        return 0;
    }
    char val[320];
    DWORD sz = sizeof(val), ty = 0;
    LONG r = RegQueryValueExA(hk, g_RunValName, NULL, &ty, (LPBYTE)val, &sz);
    RegCloseKey(hk);
    return (r == ERROR_SUCCESS && sz > 1) ? 1 : 0;
}

static void SetBootAutoStart(int enable) {
    HKEY hk = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, g_RunKeyPath, 0, KEY_WRITE, &hk) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        char exe[260], cmd[300];
        DWORD len = GetModuleFileNameA(NULL, exe, sizeof(exe) - 1);
        if (len > 0 && len < sizeof(exe)) {
            exe[len] = '\0';
            cmd[0] = '\0';
            StrAppend(cmd, sizeof(cmd), "\"");
            StrAppend(cmd, sizeof(cmd), exe);
            StrAppend(cmd, sizeof(cmd), "\" /tray");
            RegSetValueExA(hk, g_RunValName, 0, REG_SZ, (const BYTE *)cmd, StrLen(cmd) + 1);
            Print("[AUTO-START] Enabled! RTWBRIDGE will sit in Tray at boot and pop up on USB plug-in.\r\n");
        }
    } else {
        RegDeleteValueA(hk, g_RunValName);
        Print("[AUTO-START] Disabled.\r\n");
    }
    RegCloseKey(hk);
}

/* =========================================================================
 * Single-Threaded USB Bridge Worker (Runs in Native Win95 GUI Background!)
 * ========================================================================= */
static DWORD WINAPI BridgeWorkerThread(LPVOID param) {
    (void)param;
    g_WorkerActive = 1;
    g_BridgeStop   = 0;

    /* Reset per-connection crypto & counter state cleanly for reconnects */
    g_Encrypted      = 0;
    g_HandshakeState = 0;
    g_ApIsWpa2       = 0;
    g_ApChannel      = 0;
    g_Aid            = 0;
    g_TxPnLo         = 1;
    g_TxPnHi         = 0;
    g_Batch.Count    = 0;
    for (int i = 0; i < 4; i++) g_GtkValid[i] = 0;
    g_StatTxEth = g_StatRxEth = g_StatRxBcn = g_StatDecFail = 0;
    g_StatEapol = g_StatTxEcho = g_StatRxUcast = 0;

    Print("=========================================================\r\n");
    Print("  RTL8822BU Win95 Wi-Fi Bridge (Native GUI + WPA2-CCMP)\r\n");
    Print("=========================================================\r\n");

    g_TargetSsidLen = StrLen(g_TargetSsid);
    g_PasswordLen   = StrLen(g_Password);
    if (g_TargetSsidLen == 0) {
        SetStatus("Enter a Wi-Fi Name (SSID) above and click Connect.");
        g_WorkerActive = 0;
        return 1;
    }

    Print("Target SSID: \""); Print(g_TargetSsid); Print("\"");
    if (g_PasswordLen > 0) Print(" (WPA2-PSK)\r\n");
    else Print(" (OPEN)\r\n");

    /* 1. Open RTW88.VXD (without DELETE_ON_CLOSE!) & grab VTCP.386's exact MAC */
    SetStatus("Opening RTW88.VXD & RTWPROBE...");
    g_hVxd = CreateFileA("\\\\.\\RTW88.VXD", 0, 0, NULL, 0, 0, NULL);
    if (g_hVxd != INVALID_HANDLE_VALUE) {
        RTW88_DIAG diag;
        DWORD br = 0;
        c_zero(&diag, sizeof(diag));
        if (DeviceIoControl(g_hVxd, RTW88_DIOC_GET_DIAG, NULL, 0, &diag, sizeof(diag), &br, NULL) &&
            diag.Magic == 0x38385452UL) {
            c_copy(g_StaMac, diag.CurrentMac, 6);
            Print("[1/6] RTW88.VXD OK! Using VTCP MAC="); PrintMac(g_StaMac); Print("\r\n");
        }
    } else {
        Print("[1/6] WARN: \\\\.\\RTW88.VXD not open (err="); PrintDec(GetLastError()); Print(")\r\n");
    }

    /* 2. Open single serialized handle to RTWPROBE (zero concurrent USB URBs!) */
    g_hDev = CreateFileA("\\\\.\\RTWPROBE", GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (g_hDev == INVALID_HANDLE_VALUE) {
        SetStatus("USB Wi-Fi dongle not detected - plug it in and click Connect!");
        Print("[WARN] Cannot open \\\\.\\RTWPROBE (err="); PrintDec(GetLastError()); Print(")\r\n");
        goto cleanup;
    }
    g_DongleSeen = 1;

    DWORD br = 0;
    if (!DeviceIoControl(g_hDev, IOCTL_RTW_GET_PIPES, NULL, 0, &g_Pipes, sizeof(g_Pipes), &br, NULL)) {
        SetStatus("ERROR: IOCTL_RTW_GET_PIPES failed!");
        Print("[FATAL] IOCTL_RTW_GET_PIPES failed!\r\n");
        goto cleanup;
    }
    g_NumBulkOut = g_Pipes.NumBulkOut ? g_Pipes.NumBulkOut : 3;
    g_BulkOutMaxPkt = 64;
    for (u32 i = 0; i < g_Pipes.NumPipes && i < 8; i++) {
        if (!(g_Pipes.Pipes[i].EndpointAddress & 0x80) && g_Pipes.Pipes[i].PipeType == 2 &&
            g_Pipes.Pipes[i].MaxPacketSize >= 64) {
            g_BulkOutMaxPkt = g_Pipes.Pipes[i].MaxPacketSize;
            break;
        }
    }

    /* 3. Bring up RTL8822BU Radio */
    SetStatus("Bringing up RTL8822BU radio (Power + FW + MAC + PHY)...");
    Print("[2/6] Bringing up RTL8822BU Radio (Power + FW + MAC + PHY)...\r\n");
    g_CutVer = (u8)((RegRead32(0x00F0) >> 12) & 0xF);
    ReadEfuse();
    if (!RtwMacPowerOn()) { SetStatus("ERROR: Radio PowerOn failed!"); Print("  [FATAL] PowerOn failed\r\n"); goto cleanup; }
    if (!RtwDownloadFirmware()) {
        Print("  [INFO] Retrying FW download after clean power-cycle...\r\n");
        PowerSwitch(0);
        Sleep(100);
        RtwMacPowerOn();
        if (!RtwDownloadFirmware()) {
            Print("  [WARN] FW boot timeout\r\n");
        }
    }
    if (!RtwMacInit()) { SetStatus("ERROR: MacInit failed!"); Print("  [FATAL] MacInit failed\r\n"); goto cleanup; }
    RtwPhySetParam();
    Print("  Radio UP! Cut="); PrintDec(g_CutVer); Print(" RFE="); PrintDec(g_RfeOpt);
    Print(" MgmtPipe="); PrintDec(g_MgmtPipeIdx); Print(" DataPipe="); PrintDec(g_DataPipeIdx); Print("\r\n");
    if (!g_Running || g_BridgeStop) goto cleanup;

    /* 4. Precompute WPA2 PMK if password supplied */
    if (g_PasswordLen > 0) {
        SetStatus("Computing WPA2-PSK PMK (PBKDF2-SHA1)...");
        Print("[3/6] Computing WPA2-PSK PMK (4096x PBKDF2-SHA1)... ");
        pbkdf2_sha1_pmk((const u8 *)g_Password, g_PasswordLen,
                        (const u8 *)g_TargetSsid, g_TargetSsidLen, g_Pmk);
        Print("OK\r\n");
    } else {
        Print("[3/6] Open Wi-Fi mode (no PMK needed)\r\n");
    }
    if (!g_Running || g_BridgeStop) goto cleanup;

    /* 5. Locate Target AP across 2.4 GHz Channels */
    SetStatus("Scanning 2.4 GHz for target Wi-Fi network...");
    static const u8 scanCh[] = { 1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10 };
    SCAN_CTX sctx;
    sctx.found = 0;
    Print("[4/6] Searching for \""); Print(g_TargetSsid); Print("\"");
    for (int pass = 0; pass < 2 && !sctx.found && g_Running && !g_BridgeStop; pass++) {
        int chCount = g_ForceCh ? 1 : 11;
        for (int ci = 0; ci < chCount && !sctx.found && g_Running && !g_BridgeStop; ci++) {
            sctx.curCh = g_ForceCh ? g_ForceCh : scanCh[ci];
            Print(".");
            RtwSetChannel2G(sctx.curCh);
            u32 bb0 = (RegRead32(0x0F04) & 0xFFFFU) + (RegRead32(0x0F14) & 0xFFFFU);
            u32 r284_0 = RegRead32(0x0284);
            Sleep(15);
            SendProbeReq();

            DWORD tw = GetTickCount();
            int chActive = 0;
            while ((GetTickCount() - tw) < 250 && g_Running && !g_BridgeStop) {
                u32 bbNow = (RegRead32(0x0F04) & 0xFFFFU) + (RegRead32(0x0F14) & 0xFFFFU);
                u32 r284Now = RegRead32(0x0284);
                if (bbNow != bb0 || r284Now != r284_0) { chActive = 1; break; }
                Sleep(15);
            }
            if (!chActive) continue;

            DWORD td = GetTickCount();
            for (int k = 0; k < 20 && !sctx.found && (GetTickCount() - td) < 400 && g_Running && !g_BridgeStop; k++) {
                if (PollRxBatch(ScanFrameCb, &sctx) < 0) { g_BridgeStop = 1; break; }
            }
        }
    }
    Print("\r\n");
    if (!g_Running || g_BridgeStop) goto cleanup;
    if (!sctx.found) {
        SetStatus("Could not find that Wi-Fi SSID on 2.4 GHz (check name & retry).");
        Print("  [ERROR] Could not find SSID \""); Print(g_TargetSsid); Print("\" on 2.4 GHz!\r\n");
        goto cleanup;
    }

    if (g_PasswordLen > 0) g_ApIsWpa2 = 1;
    else if (g_ApIsWpa2) {
        Print("  [WARN] AP advertises WPA2 but no password was given!\r\n");
    }

    Print("  FOUND! BSSID="); PrintMac(g_Bssid);
    Print(" CH="); PrintDec(g_ApChannel);
    Print(g_ApIsWpa2 ? " WPA2" : " OPEN");
    if (g_ApIsWpa2) {
        Print(" GrpCipher="); PrintHex2(g_ApGroupCipher[3]);
        Print((g_ApGroupCipher[3] == 4) ? "(CCMP)" : "(TKIP)");
    }
    Print("\r\n");

    RtwSetChannel2G(g_ApChannel);
    for (int i = 0; i < 6; i++) {
        RegWrite8((u16)(0x0610 + i), g_StaMac[i]);
        RegWrite8((u16)(0x0618 + i), g_Bssid[i]);
    }
    RegMask8(0x0102, 0x03, 0x02);      /* RTW_NET_MGD_LINKED */
    RegWrite32(0x0608, 0xF40022CEU);   /* Filter to our BSSID + unicast/bcast/mcast */
    BatchFlush();

    /* 6. 802.11 Authentication + Association */
    SetStatus("Authenticating & associating with AP...");
    Print("[5/6] Authenticating & Associating...\r\n");
    CONN_CTX cctx;
    cctx.authOk = 0; cctx.assocOk = 0; cctx.status = 0xFFFF;

    for (int tries = 0; tries < 6 && !cctx.authOk && g_Running && !g_BridgeStop; tries++) {
        SendAuthReq();
        DWORD t0 = GetTickCount();
        while ((GetTickCount() - t0) < 500 && !cctx.authOk && g_Running && !g_BridgeStop) {
            if (PollRxBatch(ConnFrameCb, &cctx) < 0) { g_BridgeStop = 1; break; }
        }
    }
    if (!cctx.authOk) {
        SetStatus("ERROR: 802.11 Authentication failed!");
        Print("  [ERROR] Authentication failed (status="); PrintDec(cctx.status); Print(")\r\n");
        goto cleanup;
    }
    Print("  Auth OK!\r\n");

    for (int tries = 0; tries < 6 && !cctx.assocOk && g_Running && !g_BridgeStop; tries++) {
        SendAssocReq();
        DWORD t0 = GetTickCount();
        while ((GetTickCount() - t0) < 600 && !cctx.assocOk && g_Running && !g_BridgeStop) {
            if (PollRxBatch(ConnFrameCb, &cctx) < 0) { g_BridgeStop = 1; break; }
        }
    }
    if (!cctx.assocOk) {
        SetStatus("ERROR: 802.11 Association failed!");
        Print("  [ERROR] Association failed (status="); PrintDec(cctx.status); Print(")\r\n");
        goto cleanup;
    }
    RegMask32(0x06A8, 0x3FFFU, g_Aid);
    BatchFlush();
    Print("  Associated! AID="); PrintDec(g_Aid); Print("\r\n");

    /* 7. WPA2-PSK 4-Way Handshake */
    if (g_ApIsWpa2) {
        SetStatus("Performing WPA2-PSK 4-Way Handshake...");
        Print("[6/6] Waiting for WPA2 4-Way Handshake (M1..M4)...\r\n");
        DWORD t0 = GetTickCount();
        while (!g_Encrypted && (GetTickCount() - t0) < 8000 && g_Running && !g_BridgeStop) {
            if (PollRxBatch(BridgeRxCb, NULL) < 0) { g_BridgeStop = 1; break; }
        }
        if (!g_Encrypted) {
            SetStatus("WPA2 handshake failed (wrong password?) - edit & retry.");
            Print("  [ERROR] WPA2 4-Way Handshake timed out (state=");
            PrintDec((u32)g_HandshakeState); Print(" eapol="); PrintDec(g_StatEapol); Print(")\r\n");
            goto cleanup;
        }
    } else {
        Print("[6/6] Open link UP!\r\n");
    }

    FlushStaleVxdTx();
    RunTxSelfTest();

    /* Build live status banner */
    char stBuf[160];
    stBuf[0] = '\0';
    StrAppend(stBuf, sizeof(stBuf), "CONNECTED: \"");
    StrAppend(stBuf, sizeof(stBuf), g_TargetSsid);
    StrAppend(stBuf, sizeof(stBuf), "\" (CH");
    StrAppendDec(stBuf, sizeof(stBuf), g_ApChannel);
    StrAppend(stBuf, sizeof(stBuf), g_ApIsWpa2 ? " WPA2-CCMP, " : " OPEN, ");
    StrAppend(stBuf, sizeof(stBuf), (g_DataRate == 4U) ? "6 Mbps OFDM)" : "1 Mbps CCK)");
    SetStatus(stBuf);

    Print("\r\n=========================================================\r\n");
    Print("  WI-FI CONNECTED & BRIDGING IN BACKGROUND!\r\n");
    Print("  Click 'Hide to Tray' or minimize and browse the web!\r\n");
    Print("=========================================================\r\n\r\n");
    SaveLog();

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    /* Single-threaded TX/RX bridge loop on g_hDev (zero USB contention!) */
    DWORD lastStat = GetTickCount();
    DWORD lastKeepalive = GetTickCount();
    int usbErrStreak = 0;
    while (g_Running && !g_BridgeStop) {
        if (DrainVxdTx() > 0) lastKeepalive = GetTickCount();

        int rxCnt = PollRxBatch(BridgeRxCb, NULL);
        if (rxCnt < 0) {
            usbErrStreak++;
            if (usbErrStreak >= 3) {
                Print("[INFO] USB Wi-Fi dongle unplugged!\r\n");
                SetStatus("USB Wi-Fi dongle unplugged - waiting for plug-in...");
                g_DongleSeen = 0;
                break;
            }
            Sleep(25);
        } else {
            usbErrStreak = 0;
            if (rxCnt == 0) Sleep(5);
        }

        if (DrainVxdTx() > 0) lastKeepalive = GetTickCount();

        DWORD now = GetTickCount();
        if ((now - lastKeepalive) >= 12000) {
            lastKeepalive = now;
            SendNullKeepalive(g_hDev, &g_TxMain);
        }
        if ((now - lastStat) >= 10000) {
            lastStat = now;
            DWORD vxdTxQ = 0, vxdDeq = 0, vxdRxInj = 0;
            if (g_hVxd != INVALID_HANDLE_VALUE) {
                RTW88_DIAG d;
                DWORD cb = 0;
                c_zero(&d, sizeof(d));
                if (DeviceIoControl(g_hVxd, RTW88_DIOC_GET_DIAG, NULL, 0, &d, sizeof(d), &cb, NULL)) {
                    vxdTxQ = d.TxEnqueued;
                    vxdDeq = d.TxDequeued;
                    vxdRxInj = d.RxInjected;
                }
            }
            Print("[STAT] TX="); PrintDec(g_StatTxEth);
            Print(" RX="); PrintDec(g_StatRxEth);
            Print(" Ucast="); PrintDec(g_StatRxUcast);
            Print(" VxdTx="); PrintDec(vxdTxQ);
            Print("/"); PrintDec(vxdDeq);
            Print(" VxdRx="); PrintDec(vxdRxInj);
            Print(" BCN="); PrintDec(g_StatRxBcn);
            Print(" DecFail="); PrintDec(g_StatDecFail); Print("\r\n");
            SaveLog();
        }
    }

cleanup:
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    if (g_hDev != INVALID_HANDLE_VALUE) { CloseHandle(g_hDev); g_hDev = INVALID_HANDLE_VALUE; }
    if (g_hVxd != INVALID_HANDLE_VALUE) { CloseHandle(g_hVxd); g_hVxd = INVALID_HANDLE_VALUE; }
    if (g_BridgeStop && g_Running) {
        SetStatus("Disconnected. Enter Wi-Fi Name & Password and click Connect.");
        Print("[INFO] Disconnected.\r\n");
    }
    SaveLog();
    g_WorkerActive = 0;
    return 0;
}

/* =========================================================================
 * System Tray, USB Plug-In Watcher & Native Windows 95 Wi-Fi Dialog
 * ========================================================================= */
static void UpdateTrayIcon(int op, const char *tip) {
    if (!g_hWnd) return;
    WIN95_NOTIFYICONDATAA nid;
    c_zero(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid); /* Strictly 88 bytes for Windows 95 SHELL32.dll! */
    nid.hWnd   = g_hWnd;
    nid.uID    = 1;
    nid.uFlags = W95_NIF_MESSAGE | W95_NIF_ICON | W95_NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon  = g_hAppIcon;
    if (tip) {
        nid.szTip[0] = '\0';
        StrAppend(nid.szTip, sizeof(nid.szTip), tip);
    }
    Shell_NotifyIconA((DWORD)op, (PNOTIFYICONDATAA)&nid);
    if (op == W95_NIM_ADD) g_TrayAdded = 1;
    else if (op == W95_NIM_DELETE) g_TrayAdded = 0;
}

static int ProbeUsbDongle(void) {
    HANDLE h = CreateFileA("\\\\.\\RTWPROBE", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

static void ShowAndFocusMainWindow(void) {
    if (!g_hWnd) return;
    ShowWindow(g_hWnd, SW_SHOWNORMAL);
    ShowWindow(g_hWnd, SW_RESTORE);
    SetForegroundWindow(g_hWnd);
}

static void SyncControlEnableState(void) {
    BOOL busy = g_WorkerActive ? TRUE : FALSE;
    if (g_hSsidEdit) EnableWindow(g_hSsidEdit, !busy);
    if (g_hPassEdit) EnableWindow(g_hPassEdit, !busy);
    if (g_hBtnConn)  EnableWindow(g_hBtnConn,  !busy);
    if (g_hBtnDisc)  EnableWindow(g_hBtnDisc,  busy);
}

static void StartBridgeFromUi(void) {
    if (g_WorkerActive) return;

    if (g_hSsidEdit) GetWindowTextA(g_hSsidEdit, g_TargetSsid, sizeof(g_TargetSsid));
    if (g_hPassEdit) GetWindowTextA(g_hPassEdit, g_Password, sizeof(g_Password));
    g_TargetSsidLen = StrLen(g_TargetSsid);
    g_PasswordLen   = StrLen(g_Password);
    if (g_TargetSsidLen == 0) {
        SetStatus("Please enter a Wi-Fi Name (SSID) first!");
        if (g_hSsidEdit) SetFocus(g_hSsidEdit);
        return;
    }

    /* Save SSID / Password / Auto-connect preference to WIFI.TXT */
    SaveWifiConfig();

    if (g_hWorker) { CloseHandle(g_hWorker); g_hWorker = NULL; }
    g_WorkerActive = 1;
    g_BridgeStop   = 0;
    SyncControlEnableState();

    DWORD tid = 0;
    g_hWorker = CreateThread(NULL, 0x40000, BridgeWorkerThread, NULL, 0, &tid);
    if (!g_hWorker) {
        g_WorkerActive = 0;
        SyncControlEnableState();
        SetStatus("ERROR: CreateThread failed!");
    }
}

static void StopBridgeFromUi(void) {
    if (!g_WorkerActive) return;
    g_BridgeStop = 1;
    SetStatus("Disconnecting...");
}

static void PollUsbPlugIn(void) {
    if (g_WorkerActive) return; /* Never probe while worker owns \\.\RTWPROBE */
    int nowPresent = ProbeUsbDongle();
    if (nowPresent && !g_DongleSeen) {
        g_DongleSeen = 1;
        Print("[USB] RTL8822BU Wi-Fi dongle plugged in!\r\n");
        SetStatus("USB Wi-Fi dongle plugged in! Enter Wi-Fi Name & Password -> Connect.");
        UpdateTrayIcon(W95_NIM_MODIFY, "RTL8822BU Wi-Fi: Dongle Ready");
        ShowAndFocusMainWindow();
        if (g_AutoConnect && StrLen(g_TargetSsid) > 0) {
            StartBridgeFromUi();
        } else if (g_hSsidEdit) {
            SetFocus(g_hSsidEdit);
        }
    } else if (!nowPresent && g_DongleSeen) {
        g_DongleSeen = 0;
        SetStatus("Waiting for RTL8822BU USB Wi-Fi dongle (plug it in!)...");
        UpdateTrayIcon(W95_NIM_MODIFY, "RTL8822BU Wi-Fi: Waiting for USB dongle");
    }
}

static void UpdateGuiFromWorker(void) {
    SyncControlEnableState();

    if (g_StatusDirty && g_hStatus) {
        char tmp[160];
        EnterCriticalSection(&g_PrintLock);
        c_copy(tmp, g_StatusText, sizeof(tmp));
        g_StatusDirty = 0;
        LeaveCriticalSection(&g_PrintLock);
        SetWindowTextA(g_hStatus, tmp);
        if (g_TrayAdded) {
            char tip[64];
            tip[0] = '\0';
            StrAppend(tip, sizeof(tip), tmp);
            UpdateTrayIcon(W95_NIM_MODIFY, tip);
        }
    }

    if (g_hStats) {
        char sb[160];
        sb[0] = '\0';
        StrAppend(sb, sizeof(sb), "TX: ");       StrAppendDec(sb, sizeof(sb), g_StatTxEth);
        StrAppend(sb, sizeof(sb), "   |   RX: "); StrAppendDec(sb, sizeof(sb), g_StatRxEth);
        StrAppend(sb, sizeof(sb), "   |   Ucast: "); StrAppendDec(sb, sizeof(sb), g_StatRxUcast);
        StrAppend(sb, sizeof(sb), "   |   BCN: ");   StrAppendDec(sb, sizeof(sb), g_StatRxBcn);
        StrAppend(sb, sizeof(sb), "   |   DecFail: "); StrAppendDec(sb, sizeof(sb), g_StatDecFail);
        SetWindowTextA(g_hStats, sb);
    }

    if (g_LogDirty && g_hEdit) {
        u32 dispLen = 0;
        EnterCriticalSection(&g_PrintLock);
        /* Keep last ~12 KB so Win95's 30 KB EDIT control limit is never hit! */
        u32 start = 0;
        if (g_LogLen > 12000) {
            start = g_LogLen - 12000;
            while (start < g_LogLen && g_LogBuf[start] != '\n') start++;
            if (start < g_LogLen) start++;
        }
        dispLen = g_LogLen - start;
        if (dispLen >= sizeof(g_DispBuf)) dispLen = sizeof(g_DispBuf) - 1;
        c_copy(g_DispBuf, g_LogBuf + start, dispLen);
        g_DispBuf[dispLen] = '\0';
        g_LogDirty = 0;
        LeaveCriticalSection(&g_PrintLock);

        SetWindowTextA(g_hEdit, g_DispBuf);
        SendMessageA(g_hEdit, EM_SETSEL, (WPARAM)dispLen, (LPARAM)dispLen);
        SendMessageA(g_hEdit, EM_SCROLLCARET, 0, 0);
    }
}

static void LayoutControls(int w, int h) {
    if (w < 320 || h < 180) return;
    int editW = w - 285;
    if (editW < 100) editW = 100;
    int btnX1 = 105 + editW + 8;
    int btnX2 = btnX1 + 82;

    if (g_hSsidEdit) MoveWindow(g_hSsidEdit, 105,  8, editW, 21, TRUE);
    if (g_hPassEdit) MoveWindow(g_hPassEdit, 105, 34, editW, 21, TRUE);
    if (g_hBtnConn)  MoveWindow(g_hBtnConn,  btnX1,  7, 76, 23, TRUE);
    if (g_hBtnDisc)  MoveWindow(g_hBtnDisc,  btnX2,  7, 82, 23, TRUE);
    if (g_hChkShow)  MoveWindow(g_hChkShow,  btnX1, 35, 76, 20, TRUE);
    if (g_hBtnTray)  MoveWindow(g_hBtnTray,  btnX2, 33, 82, 23, TRUE);
    if (g_hChkBoot)  MoveWindow(g_hChkBoot,  10,  62, 340, 18, TRUE);
    if (g_hChkAuto)  MoveWindow(g_hChkAuto,  355, 62, 195, 18, TRUE);
    if (g_hStatus)   MoveWindow(g_hStatus,   10,  86, w - 20, 18, TRUE);
    if (g_hStats)    MoveWindow(g_hStats,    10, 106, w - 20, 18, TRUE);
    if (g_hEdit)     MoveWindow(g_hEdit,      8, 128, w - 16, h - 136, TRUE);
}

static LRESULT CALLBACK BridgeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HINSTANCE hInst = GetModuleHandleA(NULL);
            HFONT hGuiFont  = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT hFixFont  = (HFONT)GetStockObject(ANSI_FIXED_FONT);

            HWND hLblSsid = CreateWindowExA(0, "STATIC", "Wi-Fi Name:",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 11, 90, 18, hwnd, NULL, hInst, NULL);
            HWND hLblPass = CreateWindowExA(0, "STATIC", "Password:",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 37, 90, 18, hwnd, NULL, hInst, NULL);

            g_hSsidEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_TargetSsid,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                105, 8, 250, 21, hwnd, (HMENU)IDC_SSID, hInst, NULL);
            g_hPassEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_Password,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
                105, 34, 250, 21, hwnd, (HMENU)IDC_PASS, hInst, NULL);

            g_hBtnConn = CreateWindowExA(0, "BUTTON", "Connect",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                363, 7, 76, 23, hwnd, (HMENU)IDC_CONNECT, hInst, NULL);
            g_hBtnDisc = CreateWindowExA(0, "BUTTON", "Disconnect",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
                445, 7, 82, 23, hwnd, (HMENU)IDC_DISCONN, hInst, NULL);

            g_hChkShow = CreateWindowExA(0, "BUTTON", "Show",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                363, 35, 76, 20, hwnd, (HMENU)IDC_SHOWPASS, hInst, NULL);
            g_hBtnTray = CreateWindowExA(0, "BUTTON", "Hide to Tray",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                445, 33, 82, 23, hwnd, (HMENU)IDC_TOTRAY, hInst, NULL);

            g_hChkBoot = CreateWindowExA(0, "BUTTON", "Run in Tray at boot (auto-open on USB plug-in)",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                10, 62, 340, 18, hwnd, (HMENU)IDC_BOOTWATCH, hInst, NULL);
            g_hChkAuto = CreateWindowExA(0, "BUTTON", "Auto-connect on plug-in",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                355, 62, 195, 18, hwnd, (HMENU)IDC_AUTOCONN, hInst, NULL);

            g_hStatus = CreateWindowExA(0, "STATIC", "Ready. Plug in USB Wi-Fi dongle and click Connect.",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 86, 530, 18, hwnd, (HMENU)IDC_STATUS, hInst, NULL);
            g_hStats = CreateWindowExA(0, "STATIC", "TX: 0   |   RX: 0   |   Ucast: 0   |   BCN: 0   |   DecFail: 0",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 106, 530, 18, hwnd, (HMENU)IDC_STATS, hInst, NULL);
            g_hEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                8, 128, 536, 260, hwnd, (HMENU)IDC_LOG, hInst, NULL);

            if (hGuiFont) {
                HWND ctrls[] = { hLblSsid, hLblPass, g_hSsidEdit, g_hPassEdit,
                                 g_hBtnConn, g_hBtnDisc, g_hChkShow, g_hBtnTray,
                                 g_hChkBoot, g_hChkAuto, g_hStatus, g_hStats };
                for (int i = 0; i < (int)(sizeof(ctrls) / sizeof(ctrls[0])); i++) {
                    if (ctrls[i]) SendMessageA(ctrls[i], WM_SETFONT, (WPARAM)hGuiFont, TRUE);
                }
            }
            if (hFixFont && g_hEdit) {
                SendMessageA(g_hEdit, WM_SETFONT, (WPARAM)hFixFont, TRUE);
            }

            if (IsBootAutoStartEnabled()) {
                SendMessageA(g_hChkBoot, BM_SETCHECK, BST_CHECKED, 0);
            }
            if (g_AutoConnect) {
                SendMessageA(g_hChkAuto, BM_SETCHECK, BST_CHECKED, 0);
            }

            SetTimer(hwnd, 1, 250, NULL);   /* UI log/stats refresh */
            SetTimer(hwnd, 2, 1500, NULL);  /* USB plug-in watcher */
            return 0;
        }
        case WM_SIZE:
            LayoutControls((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDC_CONNECT || id == IDOK) {
                StartBridgeFromUi();
                return 0;
            } else if (id == IDC_DISCONN) {
                StopBridgeFromUi();
                return 0;
            } else if (id == IDC_SHOWPASS) {
                int show = (SendMessageA(g_hChkShow, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                SendMessageA(g_hPassEdit, EM_SETPASSWORDCHAR, show ? 0 : (WPARAM)'*', 0);
                InvalidateRect(g_hPassEdit, NULL, TRUE);
                return 0;
            } else if (id == IDC_TOTRAY) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            } else if (id == IDC_BOOTWATCH) {
                int en = (SendMessageA(g_hChkBoot, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                SetBootAutoStart(en);
                return 0;
            } else if (id == IDC_AUTOCONN) {
                g_AutoConnect = (SendMessageA(g_hChkAuto, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                if (g_hSsidEdit) GetWindowTextA(g_hSsidEdit, g_TargetSsid, sizeof(g_TargetSsid));
                if (g_hPassEdit) GetWindowTextA(g_hPassEdit, g_Password, sizeof(g_Password));
                SaveWifiConfig();
                return 0;
            } else if (id == IDM_TRAY_OPEN) {
                ShowAndFocusMainWindow();
                return 0;
            } else if (id == IDM_TRAY_EXIT) {
                SendMessageA(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        }
        case WM_TRAYICON:
            if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
                if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
                else ShowAndFocusMainWindow();
            } else if (lp == WM_RBUTTONUP) {
                HMENU hm = CreatePopupMenu();
                if (hm) {
                    POINT pt;
                    GetCursorPos(&pt);
                    AppendMenuA(hm, MF_STRING, IDM_TRAY_OPEN, "Open RTL8822BU Wi-Fi Bridge");
                    AppendMenuA(hm, MF_SEPARATOR, 0, NULL);
                    AppendMenuA(hm, MF_STRING, IDM_TRAY_EXIT, "Exit");
                    SetForegroundWindow(hwnd);
                    TrackPopupMenu(hm, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
                    DestroyMenu(hm);
                }
            }
            return 0;
        case WM_DEVICECHANGE:
            PollUsbPlugIn();
            return 0;
        case WM_TIMER:
            if (wp == 1) UpdateGuiFromWorker();
            else if (wp == 2) PollUsbPlugIn();
            return 0;
        case WM_CLOSE:
            g_Running = 0;
            g_BridgeStop = 1;
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 2);
            UpdateTrayIcon(W95_NIM_DELETE, NULL);
            SaveLog();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void WinMainCRTStartup(void) {
    /* Single-instance check: if already running in tray, just pop it up! */
    HWND hPrev = FindWindowA("RTW88Win95Gui", NULL);
    if (hPrev) {
        ShowWindow(hPrev, SW_SHOWNORMAL);
        ShowWindow(hPrev, SW_RESTORE);
        SetForegroundWindow(hPrev);
        ExitProcess(0);
    }

    InitializeCriticalSection(&g_Lock);
    InitializeCriticalSection(&g_PrintLock);
    InitializeCriticalSection(&g_VxdLock);
    InitializeCriticalSection(&g_TxLock);

    LoadConfig();

    HINSTANCE hInst = GetModuleHandleA(NULL);
    g_hAppIcon = LoadIconA(NULL, IDI_APPLICATION);

    WNDCLASSA wc;
    c_zero(&wc, sizeof(wc));
    wc.lpfnWndProc   = BridgeWndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = g_hAppIcon;
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "RTW88Win95Gui";
    RegisterClassA(&wc);

    g_hWnd = CreateWindowExA(
        0, "RTW88Win95Gui", "RTL8822BU Win95 Wi-Fi Bridge",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 560, 420,
        NULL, NULL, hInst, NULL);
    if (!g_hWnd) ExitProcess(1);

    UpdateTrayIcon(W95_NIM_ADD, "RTL8822BU Win95 Wi-Fi Bridge");

    int dongleNow = ProbeUsbDongle();
    g_DongleSeen = dongleNow;

    if (g_StartInTray && !dongleNow) {
        /* Booted into Tray with dongle unplugged: stay hidden until plugged in! */
        SetStatus("Waiting for RTL8822BU USB Wi-Fi dongle (plug it in!)...");
        UpdateTrayIcon(W95_NIM_MODIFY, "RTL8822BU Wi-Fi: Waiting for USB dongle");
        ShowWindow(g_hWnd, SW_HIDE);
    } else {
        ShowWindow(g_hWnd, SW_SHOWNORMAL);
        UpdateWindow(g_hWnd);
        if (dongleNow) {
            if ((g_CliConnect || g_AutoConnect) && g_TargetSsidLen > 0) {
                StartBridgeFromUi();
            } else {
                SetStatus("USB Wi-Fi dongle ready! Enter Wi-Fi Name & Password and click Connect.");
                if (g_hSsidEdit) SetFocus(g_hSsidEdit);
            }
        } else {
            SetStatus("Waiting for RTL8822BU USB Wi-Fi dongle (plug it in!)...");
        }
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(g_hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    g_Running = 0;
    g_BridgeStop = 1;
    UpdateTrayIcon(W95_NIM_DELETE, NULL);
    SaveLog();
    ExitProcess(0);
}
