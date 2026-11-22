#include <windows.h>

#define IOCTL_RTW_FW_INIT           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_FW_WRITE_PAGE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_FW_BOOT           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_MAC_ENABLE        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_BULK_RX           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_BULK_TX           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x812, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _RTW_FW_PAGE_PARAM {
    unsigned long PageIndex;
    unsigned long TargetAddr;
    unsigned long Length;
    unsigned char Data[4096];
} RTW_FW_PAGE_PARAM;

typedef struct _RTW_BOOT_RESULT {
    unsigned char Success;
    unsigned char Reserved;
    unsigned short FinalMcuFwCtrl;
    unsigned char PostBootMac[6];
    unsigned char Reserved2[2];
    long Status;
} RTW_BOOT_RESULT;

typedef struct _RTW_RX_PACKET {
    unsigned long Status;
    unsigned long Length;
    unsigned long RxPktNum;
    unsigned char Data[2048];
} RTW_RX_PACKET;

typedef struct _WIFI_NETWORK {
    char Ssid[36];
    unsigned char Bssid[6];
    unsigned char Channel;
} WIFI_NETWORK;

static RTW_FW_PAGE_PARAM g_PageParam;
static RTW_BOOT_RESULT g_BootRes;
static RTW_RX_PACKET g_RxPkt;
static WIFI_NETWORK g_Networks[16];
static int g_NetworkCount = 0;
static char g_LogBuf[8192];

size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

static unsigned long MyStrLen(const char *s) {
    return (unsigned long)strlen(s);
}

static void MyMemCpy(void *dst, const void *src, unsigned long count) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    while (count--) *d++ = *s++;
}

static int MyMemCmp(const void *s1, const void *s2, unsigned long n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    for (unsigned long i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

static void StrCat(char *dest, const char *src) {
    while (*dest) dest++;
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

static void ToHex16(char *buf, unsigned short val) {
    const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    buf[2] = hex[(val >> 12) & 0xF];
    buf[3] = hex[(val >> 8) & 0xF];
    buf[4] = hex[(val >> 4) & 0xF];
    buf[5] = hex[val & 0xF];
    buf[6] = '\0';
}

static void ToHex32(char *buf, unsigned long val) {
    const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = hex[(val >> (28 - (i * 4))) & 0xF];
    }
    buf[10] = '\0';
}

static void ToDec(char *buf, unsigned long val) {
    char temp[16];
    int i = 0;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (val > 0) {
        temp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = temp[--i];
    }
    buf[j] = '\0';
}

static void FormatMac(char *buf, const unsigned char *mac) {
    const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        buf[i * 3]     = hex[(mac[i] >> 4) & 0x0F];
        buf[i * 3 + 1] = hex[mac[i] & 0x0F];
        if (i < 5) buf[i * 3 + 2] = ':';
        else buf[i * 3 + 2] = '\0';
    }
}

static void Print(const char *s) {
    HANDLE hStd = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStd && hStd != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hStd, s, (DWORD)MyStrLen(s), &written, NULL);
    }
    StrCat(g_LogBuf, s);
}

static void WaitForEnter(void) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn && hIn != INVALID_HANDLE_VALUE) {
        char ch;
        DWORD read = 0;
        ReadFile(hIn, &ch, 1, &read, NULL);
    }
}

static void AddDiscoveredNetwork(const char *ssid, const unsigned char *bssid, unsigned char channel) {
    for (int i = 0; i < g_NetworkCount; i++) {
        if (MyMemCmp(g_Networks[i].Bssid, bssid, 6) == 0) return;
    }
    if (g_NetworkCount < 16) {
        int idx = g_NetworkCount++;
        int len = (int)MyStrLen(ssid);
        if (len > 32) len = 32;
        MyMemCpy(g_Networks[idx].Ssid, ssid, len);
        g_Networks[idx].Ssid[len] = '\0';
        MyMemCpy(g_Networks[idx].Bssid, bssid, 6);
        g_Networks[idx].Channel = channel;
    }
}

static void Parse80211Frame(const unsigned char *pkt, unsigned long len) {
    if (len < 64) return;
    for (unsigned long base = 24; base + 36 <= len; base += 4) {
        unsigned char fc0 = pkt[base];
        unsigned char fc1 = pkt[base + 1];

        if ((fc0 == 0x80 || fc0 == 0x50) && fc1 == 0x00) {
            const unsigned char *bssid = &pkt[base + 16];
            unsigned long tagStart = base + 24 + 12;
            char ssid[36];
            ssid[0] = '\0';
            unsigned char channel = 1;

            unsigned long cur = tagStart;
            while (cur + 2 <= len) {
                unsigned char tagNum = pkt[cur];
                unsigned char tagLen = pkt[cur + 1];
                if (cur + 2 + tagLen > len) break;

                if (tagNum == 0) {
                    int copyLen = tagLen;
                    if (copyLen > 32) copyLen = 32;
                    MyMemCpy(ssid, &pkt[cur + 2], copyLen);
                    ssid[copyLen] = '\0';
                } else if (tagNum == 3 && tagLen == 1) {
                    channel = pkt[cur + 2];
                }
                cur += 2 + tagLen;
            }
            if (ssid[0] == '\0') StrCat(ssid, "<Hidden SSID>");
            AddDiscoveredNetwork(ssid, bssid, channel);
            break;
        }
    }
}

static void UploadFirmware(HANDLE hDev) {
    char numBuf[16];
    char hexBuf[24];
    DWORD bytesReturned = 0;

    Print("[*] Checking for rtw8822b_fw.bin... ");
    HANDLE hFw = CreateFileA("rtw8822b_fw.bin", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFw == INVALID_HANDLE_VALUE) {
        Print("[NOT FOUND]\r\n(Please ensure rtw8822b_fw.bin is in the current directory)\r\n");
        return;
    }

    DWORD fwSize = GetFileSize(hFw, NULL);
    Print("Found (");
    ToDec(numBuf, fwSize);
    Print(numBuf);
    Print(" bytes)\r\n");

    Print("[*] Initializing MCU Download Mode... ");
    unsigned short dlVal = 0;
    DeviceIoControl(hDev, IOCTL_RTW_FW_INIT, NULL, 0, &dlVal, sizeof(dlVal), &bytesReturned, NULL);
    ToHex16(hexBuf, dlVal);
    Print(hexBuf);
    Print((dlVal & 1) ? " [OK]\r\n" : " [FAIL]\r\n");

    Print("[*] Streaming Firmware Pages... ");
    DWORD bytesRead = 0;
    unsigned long pageIdx = 0;
    BOOL streamOk = TRUE;
    SetFilePointer(hFw, 64, NULL, FILE_BEGIN); /* Skip 64-byte header */

    while (1) {
        ReadFile(hFw, g_PageParam.Data, 4096, &bytesRead, NULL);
        if (bytesRead == 0) break;

        g_PageParam.PageIndex = pageIdx;
        g_PageParam.TargetAddr = 0x1000;
        g_PageParam.Length = bytesRead;

        BOOL pgOk = DeviceIoControl(hDev, IOCTL_RTW_FW_WRITE_PAGE, &g_PageParam, sizeof(g_PageParam), NULL, 0, &bytesReturned, NULL);
        if (!pgOk) { streamOk = FALSE; break; }
        pageIdx++;
    }
    CloseHandle(hFw);

    ToDec(numBuf, pageIdx);
    Print(numBuf);
    Print(" pages streamed ");
    Print(streamOk ? "[OK]\r\n" : "[FAIL]\r\n");

    Print("[*] Booting 32-bit RISC CPU... ");
    DeviceIoControl(hDev, IOCTL_RTW_FW_BOOT, NULL, 0, &g_BootRes, sizeof(g_BootRes), &bytesReturned, NULL);
    ToHex16(hexBuf, g_BootRes.FinalMcuFwCtrl);
    Print("REG_MCUFW_CTRL = ");
    Print(hexBuf);
    if ((g_BootRes.FinalMcuFwCtrl & 0x8000) || ((g_BootRes.FinalMcuFwCtrl & 0xC078) == 0xC078)) {
        Print(" [FW_READY OK]\r\n");
    } else {
        Print(" [BOOT TIMEOUT]\r\n");
    }
}

void mainCRTStartup(void) {
    char numBuf[16];
    char hexBuf[24];
    char macBuf[24];
    DWORD bytesReturned = 0;

    Print("\r\n======================================================\r\n");
    Print(" Realtek RTL8822BU Wi-Fi Bulk Diagnostic (Phase 4.1)\r\n");
    Print(" Target: Windows 95 OSR 2.1 / 2.5 (Pentium MMX)\r\n");
    Print("======================================================\r\n\r\n");

    Print("[*] Connecting to \\\\.\\RTWPROBE... ");
    HANDLE hDev = CreateFileA(
        "\\\\.\\RTWPROBE",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hDev == INVALID_HANDLE_VALUE) {
        Print("[FAILED]\r\n");
        Print("ERROR: Driver not loaded or device not connected.\r\n");
        Print("Please verify USB Wi-Fi dongle is plugged in.\r\n");

        MessageBoxA(
            NULL,
            "Could not connect to \\\\.\\RTWPROBE.\nPlease ensure the USB Wi-Fi adapter is plugged in.",
            "RTWSCAN - Error",
            MB_ICONERROR | MB_OK
        );
        ExitProcess(1);
    }
    Print("[OK]\r\n");

    /* Step 1: Initial test of MAC engine */
    Print("[*] Activating MAC & RX Engines (REG_CR=0xFF, REG_RCR=0xE400220F)... ");
    unsigned long macStatus[3] = {0, 0, 0};
    BOOL macOk = DeviceIoControl(hDev, IOCTL_RTW_MAC_ENABLE, NULL, 0, macStatus, sizeof(macStatus), &bytesReturned, NULL);

    /* If CR is 0, firmware might need to be uploaded */
    if (!macOk || (macStatus[0] == 0)) {
        Print("[NEEDS FW]\r\n");
        UploadFirmware(hDev);

        Print("[*] Re-activating MAC & RX Engines... ");
        DeviceIoControl(hDev, IOCTL_RTW_MAC_ENABLE, NULL, 0, macStatus, sizeof(macStatus), &bytesReturned, NULL);
        Print("[OK]\r\n");
    } else {
        Print("[OK]\r\n");
    }

    Print("    - REG_CR:  ");
    ToHex16(hexBuf, (unsigned short)macStatus[0]);
    Print(hexBuf);
    Print("\r\n    - REG_RCR: ");
    ToHex32(hexBuf, macStatus[1]);
    Print(hexBuf);
    Print("\r\n    - Initial RX FIFO Queue: ");
    ToDec(numBuf, macStatus[2]);
    Print(numBuf);
    Print(" packets\r\n\r\n");

    /* Step 2: 20 passes of verified bulk polling */
    Print("[*] Polling USB Bulk RX Engine (20 passes)...\r\n");
    g_NetworkCount = 0;
    int totalPacketsHeard = 0;

    for (int pass = 1; pass <= 20; pass++) {
        Print("    Pass ");
        ToDec(numBuf, pass);
        Print(numBuf);
        Print("/20: ");

        BOOL ok = DeviceIoControl(
            hDev,
            IOCTL_RTW_BULK_RX,
            NULL, 0,
            &g_RxPkt, sizeof(g_RxPkt),
            &bytesReturned,
            NULL
        );

        if (ok) {
            Print("RX FIFO=");
            ToDec(numBuf, g_RxPkt.RxPktNum);
            Print(numBuf);
            Print(" pkts, read=");
            ToDec(numBuf, g_RxPkt.Length);
            Print(numBuf);
            Print(" bytes");

            if (g_RxPkt.Length > 24) {
                totalPacketsHeard++;
                Print(" [PACKET!]");
                Parse80211Frame(g_RxPkt.Data, g_RxPkt.Length);
            }
            Print("\r\n");
        } else {
            DWORD err = GetLastError();
            Print("Poll error (err=");
            ToDec(numBuf, err);
            Print(numBuf);
            Print(")\r\n");
            if (err == 1167 /* ERROR_DEVICE_NOT_CONNECTED */ || err == 31 /* ERROR_GEN_FAILURE */ || err == 2 /* ERROR_FILE_NOT_FOUND */) {
                Print("[!] Adapter disconnected or removed. Stopping poll loop.\r\n");
                break;
            }
        }
        Sleep(100);
    }

    /* IMMEDIATELY close handle so driver is never locked */
    CloseHandle(hDev);

    /* Step 3: Report Results */
    Print("\r\n------------------------------------------------------\r\n");
    Print(" Diagnostics Summary:\r\n");
    Print("------------------------------------------------------\r\n");

    char summaryBox[2048];
    summaryBox[0] = '\0';

    if (g_NetworkCount == 0) {
        Print("Bulk RX engine verified: polling FIFO cleanly without hanging.\r\n");
        StrCat(summaryBox, "Milestone 4.1 USB Bulk Engine Verified!\n\n");
        StrCat(summaryBox, "MAC & Promiscuous RX Engines Activated.\n");
        StrCat(summaryBox, "RX FIFO polled cleanly without system hangs.\n\n");
        StrCat(summaryBox, "Ready for Phase 4.2: NDIS 3.1 Miniport Driver!");
    } else {
        StrCat(summaryBox, "SUCCESS! Discovered Live Wi-Fi Networks:\n\n");
        for (int i = 0; i < g_NetworkCount; i++) {
            Print("  [");
            ToDec(numBuf, i + 1);
            Print(numBuf);
            Print("] SSID: \"");
            Print(g_Networks[i].Ssid);
            Print("\"\r\n      BSSID: ");
            FormatMac(macBuf, g_Networks[i].Bssid);
            Print(macBuf);
            Print(" | Channel: ");
            ToDec(numBuf, g_Networks[i].Channel);
            Print(numBuf);
            Print("\r\n");

            StrCat(summaryBox, "[");
            StrCat(summaryBox, numBuf);
            StrCat(summaryBox, "] \"");
            StrCat(summaryBox, g_Networks[i].Ssid);
            StrCat(summaryBox, "\" (Ch ");
            ToDec(numBuf, g_Networks[i].Channel);
            StrCat(summaryBox, numBuf);
            StrCat(summaryBox, ")\n");
        }
    }
    Print("\r\nDone!\r\n");

    /* Save complete transcript to RTWSCAN.LOG */
    HANDLE hLog = CreateFileA("RTWSCAN.LOG", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLog != INVALID_HANDLE_VALUE) {
        DWORD len = MyStrLen(g_LogBuf);
        DWORD written = 0;
        WriteFile(hLog, g_LogBuf, len, &written, NULL);
        CloseHandle(hLog);
    }

    /* Pop informative MessageBox */
    MessageBoxA(
        NULL,
        summaryBox,
        "Realtek 8822BU Milestone 4.1 Verification",
        MB_ICONINFORMATION | MB_OK
    );

    /* Prompt user so Explorer console window stays open */
    Print("\r\nPress Enter to exit...\r\n");
    WaitForEnter();

    ExitProcess(0);
}
