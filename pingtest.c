/*
 * pingtest.c - Milestone 6 Live Packet Pipeline Verification Tool
 *
 * 1. Verifies BOTH drivers are alive simultaneously:
 *      - \\.\RTW88.VXD (NDIS 3.1 VxD TCP/IP interface)
 *      - \\.\RTWPROBE  (WDM USB RTL8822BU hardware driver)
 * 2. Acts as a virtual gateway router (MAC 02:00:5E:95:95:01):
 *      - Pulls real outbound Ethernet frames from VTCP.386 via DIOC 2 (POP_TX)
 *      - Answers ANY ARP Request with a valid ARP Reply via DIOC 3 (INJECT_RX)
 *      - Answers ANY ICMP Echo Request ('ping 192.168.1.1') with a valid ICMP
 *        Echo Reply via DIOC 3 (INJECT_RX)!
 */
#include <windows.h>

#define RTW88_DIOC_GET_DIAG   1
#define RTW88_DIOC_POP_TX     2
#define RTW88_DIOC_INJECT_RX  3
#define RTW88_DIOC_SET_MAC    4

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

static const BYTE g_RouterMac[6] = { 0x02, 0x00, 0x5E, 0x95, 0x95, 0x01 };
static BYTE g_TxBuf[1600];
static BYTE g_RxBuf[1600];

static HANDLE g_hOut = INVALID_HANDLE_VALUE;

static void Print(const char *s) {
    DWORD len = (DWORD)lstrlenA(s);
    DWORD w = 0;
    if (g_hOut != INVALID_HANDLE_VALUE && len > 0) {
        WriteFile(g_hOut, s, len, &w, NULL);
    }
}

static void PrintDec(DWORD v) {
    char buf[16], rev[16];
    int n = 0, i;
    if (v == 0) { Print("0"); return; }
    while (v > 0 && n < 15) {
        rev[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++) buf[i] = rev[n - 1 - i];
    buf[n] = '\0';
    Print(buf);
}

static void PrintHex(DWORD v) {
    static const char h[] = "0123456789ABCDEF";
    char buf[11];
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++) buf[2 + i] = h[(v >> (28 - i * 4)) & 0xF];
    buf[10] = '\0';
    Print(buf);
}

static void PrintIp(const BYTE *ip) {
    PrintDec(ip[0]); Print(".");
    PrintDec(ip[1]); Print(".");
    PrintDec(ip[2]); Print(".");
    PrintDec(ip[3]);
}

static void PrintMac(const BYTE *m) {
    static const char h[] = "0123456789ABCDEF";
    char buf[18];
    int i;
    for (i = 0; i < 6; i++) {
        buf[i * 3]     = h[(m[i] >> 4) & 0xF];
        buf[i * 3 + 1] = h[m[i] & 0xF];
        buf[i * 3 + 2] = (i < 5) ? ':' : '\0';
    }
    Print(buf);
}

static void MemCpy(void *dst, const void *src, DWORD n) {
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--) *d++ = *s++;
}

static WORD Checksum16(const BYTE *data, DWORD len) {
    DWORD sum = 0;
    DWORD i = 0;
    while (i + 1 < len) {
        WORD w = (WORD)(((WORD)data[i] << 8) | data[i + 1]);
        sum += w;
        i += 2;
    }
    if (i < len) {
        sum += (WORD)((WORD)data[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (WORD)(~sum & 0xFFFF);
}

static void HandleArp(HANDLE hVxd, const BYTE *pkt, DWORD len) {
    if (len < 42) return;
    /* Check ARP Request: HW=Ethernet(1), Proto=IPv4(0x0800), Op=1 (Request) */
    if (pkt[14] != 0x00 || pkt[15] != 0x01) return;
    if (pkt[16] != 0x08 || pkt[17] != 0x00) return;
    if (pkt[20] != 0x00 || pkt[21] != 0x01) return;

    const BYTE *senderMac = pkt + 22;
    const BYTE *senderIp  = pkt + 28;
    const BYTE *targetIp  = pkt + 38;

    /* Ignore gratuitous ARP asking for our own IP */
    if (senderIp[0] == targetIp[0] && senderIp[1] == targetIp[1] &&
        senderIp[2] == targetIp[2] && senderIp[3] == targetIp[3]) {
        return;
    }

    Print("[TX] ARP Request: Who has "); PrintIp(targetIp);
    Print("? Tell "); PrintIp(senderIp); Print("\r\n");

    /* Build 60-byte Ethernet ARP Reply */
    DWORD i;
    for (i = 0; i < 60; i++) g_RxBuf[i] = 0;

    MemCpy(g_RxBuf + 0, senderMac, 6);      /* Dst MAC = Win95 */
    MemCpy(g_RxBuf + 6, g_RouterMac, 6);    /* Src MAC = Virtual Router */
    g_RxBuf[12] = 0x08; g_RxBuf[13] = 0x06; /* EtherType = ARP */

    g_RxBuf[14] = 0x00; g_RxBuf[15] = 0x01; /* HTYPE = Ethernet */
    g_RxBuf[16] = 0x08; g_RxBuf[17] = 0x00; /* PTYPE = IPv4 */
    g_RxBuf[18] = 6;    g_RxBuf[19] = 4;    /* HLEN=6, PLEN=4 */
    g_RxBuf[20] = 0x00; g_RxBuf[21] = 0x02; /* OPER = Reply (2) */

    MemCpy(g_RxBuf + 22, g_RouterMac, 6);   /* Sender MAC = Virtual Router */
    MemCpy(g_RxBuf + 28, targetIp, 4);      /* Sender IP  = Requested IP */
    MemCpy(g_RxBuf + 32, senderMac, 6);     /* Target MAC = Win95 */
    MemCpy(g_RxBuf + 38, senderIp, 4);      /* Target IP  = Win95 */

    DWORD cb = 0;
    if (DeviceIoControl(hVxd, RTW88_DIOC_INJECT_RX, g_RxBuf, 60, NULL, 0, &cb, NULL)) {
        Print("  -> [RX] Injected ARP Reply ("); PrintIp(targetIp);
        Print(" is at "); PrintMac(g_RouterMac); Print(")\r\n");
    } else {
        Print("  -> [RX] Inject FAILED err="); PrintDec(GetLastError()); Print("\r\n");
    }
}

static void HandleIpv4(HANDLE hVxd, const BYTE *pkt, DWORD len) {
    if (len < 34) return;
    BYTE verIhl = pkt[14];
    if ((verIhl >> 4) != 4) return;
    DWORD ihl = (DWORD)(verIhl & 0x0F) * 4;
    if (ihl < 20 || 14 + ihl + 8 > len) return;

    BYTE proto = pkt[14 + 9];
    if (proto != 1) return; /* ICMP only */

    const BYTE *icmp = pkt + 14 + ihl;
    DWORD ipTotalLen = ((DWORD)pkt[14 + 2] << 8) | pkt[14 + 3];
    if (ipTotalLen < ihl + 8 || 14 + ipTotalLen > len) return;
    DWORD icmpLen = ipTotalLen - ihl;

    if (icmp[0] != 8) return; /* Type 8 = Echo Request */

    WORD seq = (WORD)(((WORD)icmp[6] << 8) | icmp[7]);
    Print("[TX] ICMP Echo Request "); PrintIp(pkt + 14 + 12);
    Print(" -> "); PrintIp(pkt + 14 + 16);
    Print(" seq="); PrintDec(seq);
    Print(" len="); PrintDec(icmpLen); Print("\r\n");

    DWORD replyLen = 14 + ipTotalLen;
    if (replyLen > 1514) return;
    MemCpy(g_RxBuf, pkt, replyLen);

    /* Swap Ethernet MACs */
    MemCpy(g_RxBuf + 0, pkt + 6, 6);
    MemCpy(g_RxBuf + 6, g_RouterMac, 6);

    /* Swap IPv4 src/dst, set TTL=64, recompute IP header checksum */
    MemCpy(g_RxBuf + 14 + 12, pkt + 14 + 16, 4);
    MemCpy(g_RxBuf + 14 + 16, pkt + 14 + 12, 4);
    g_RxBuf[14 + 8]  = 64;
    g_RxBuf[14 + 10] = 0;
    g_RxBuf[14 + 11] = 0;
    WORD ipCsum = Checksum16(g_RxBuf + 14, ihl);
    g_RxBuf[14 + 10] = (BYTE)(ipCsum >> 8);
    g_RxBuf[14 + 11] = (BYTE)(ipCsum & 0xFF);

    /* Set ICMP Type=0 (Echo Reply), recompute ICMP checksum */
    BYTE *rIcmp = g_RxBuf + 14 + ihl;
    rIcmp[0] = 0;
    rIcmp[2] = 0;
    rIcmp[3] = 0;
    WORD icmpCsum = Checksum16(rIcmp, icmpLen);
    rIcmp[2] = (BYTE)(icmpCsum >> 8);
    rIcmp[3] = (BYTE)(icmpCsum & 0xFF);

    DWORD padLen = (replyLen < 60) ? 60 : replyLen;
    if (padLen > replyLen) {
        DWORD i;
        for (i = replyLen; i < padLen; i++) g_RxBuf[i] = 0;
    }

    DWORD cb = 0;
    if (DeviceIoControl(hVxd, RTW88_DIOC_INJECT_RX, g_RxBuf, padLen, NULL, 0, &cb, NULL)) {
        Print("  -> [RX] Injected ICMP Echo Reply seq="); PrintDec(seq); Print("\r\n");
    } else {
        Print("  -> [RX] Inject FAILED err="); PrintDec(GetLastError()); Print("\r\n");
    }
}

void mainCRTStartup(void) {
    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    Print("=====================================================\r\n");
    Print("  RTW88 Milestone 6 - Live Packet Pipeline Test\r\n");
    Print("=====================================================\r\n\r\n");

    /* 1. Check RTWPROBE.SYS (USB radio driver) */
    HANDLE hUsb = CreateFileA("\\\\.\\RTWPROBE", GENERIC_READ | GENERIC_WRITE,
                              0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hUsb != INVALID_HANDLE_VALUE) {
        Print("[OK] \\\\.\\RTWPROBE (USB Radio Driver) is OPEN!\r\n");
        CloseHandle(hUsb);
    } else {
        Print("[--] \\\\.\\RTWPROBE not open (err="); PrintDec(GetLastError());
        Print(") - ok if dongle unplugged or not in Dual Mode\r\n");
    }

    /* 2. Open RTW88.VXD (NDIS 3.1 VxD) */
    HANDLE hVxd = CreateFileA("\\\\.\\RTW88.VXD", 0, 0, NULL, 0, 0, NULL);
    if (hVxd == INVALID_HANDLE_VALUE) {
        Print("[ERR] Failed to open \\\\.\\RTW88.VXD (err="); PrintDec(GetLastError()); Print(")\r\n");
        ExitProcess(1);
    }
    Print("[OK] \\\\.\\RTW88.VXD (NDIS 3.1 VxD) is OPEN!\r\n");

    RTW88_DIAG diag;
    DWORD cb = 0;
    if (DeviceIoControl(hVxd, RTW88_DIOC_GET_DIAG, NULL, 0, &diag, sizeof(diag), &cb, NULL)) {
        Print("     MAC="); PrintMac(diag.CurrentMac);
        Print(" AddAdp="); PrintDec(diag.AddAdapterCalls);
        Print(" Opens="); PrintDec(diag.OpenAdapterCalls);
        Print(" PktMem="); PrintHex(diag.PktMemAddr); Print("\r\n");
        Print("     TxQ="); PrintDec(diag.TxEnqueued);
        Print(" TxDeq="); PrintDec(diag.TxDequeued);
        Print(" RxInj="); PrintDec(diag.RxInjected);
        Print(" Xfer="); PrintDec(diag.TransferDataCalls); Print("\r\n\r\n");
    }

    Print("Virtual Gateway active! Leave this window open and run:\r\n");
    Print("    ping 192.168.1.1\r\n");
    Print("in another MS-DOS Prompt! (Press Ctrl+C to quit)\r\n\r\n");

    for (;;) {
        DWORD pktLen = 0;
        if (DeviceIoControl(hVxd, RTW88_DIOC_POP_TX, NULL, 0, g_TxBuf, sizeof(g_TxBuf), &pktLen, NULL) &&
            pktLen >= 14) {
            WORD ethType = (WORD)(((WORD)g_TxBuf[12] << 8) | g_TxBuf[13]);
            if (ethType == 0x0806) {
                HandleArp(hVxd, g_TxBuf, pktLen);
            } else if (ethType == 0x0800) {
                HandleIpv4(hVxd, g_TxBuf, pktLen);
            } else {
                Print("[TX] Frame len="); PrintDec(pktLen);
                Print(" type="); PrintHex(ethType); Print("\r\n");
            }
        } else {
            Sleep(5);
        }
    }
}
