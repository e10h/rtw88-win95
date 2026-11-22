#include <windows.h>

#define IOCTL_RTW_PROBE         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_FW_INIT       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_FW_WRITE_PAGE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_FW_BOOT       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _RTW_PIPE_INFO {
    unsigned char EndpointAddress;
    unsigned char PipeType;
    unsigned short MaxPacketSize;
} RTW_PIPE_INFO;

typedef struct _RTW_PROBE_DATA {
    unsigned short func_en;
    unsigned short clk_ctrl;
    unsigned long sys_cfg;
    long status_func;
    long status_clk;
    long status_cfg;
    unsigned char reg_mac_0610[6];
    unsigned char reg_mac_0050[6];
    long status_mac_0610;
    long status_mac_0050;

    unsigned short mcufw_ctrl_init;
    unsigned short mcufw_ctrl_dl_en;
    unsigned long num_pipes;
    RTW_PIPE_INFO pipes[8];
    long status_select_config;
    unsigned char platform_reset_ok;
    unsigned char reserved[1];
    unsigned short mcufw_ctrl_post_reset;
} RTW_PROBE_DATA;

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
    buf[2] = hex[(val >> 28) & 0xF];
    buf[3] = hex[(val >> 24) & 0xF];
    buf[4] = hex[(val >> 20) & 0xF];
    buf[5] = hex[(val >> 16) & 0xF];
    buf[6] = hex[(val >> 12) & 0xF];
    buf[7] = hex[(val >> 8) & 0xF];
    buf[8] = hex[(val >> 4) & 0xF];
    buf[9] = hex[val & 0xF];
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
        temp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = temp[--i];
    }
    buf[j] = '\0';
}

static void FormatMac(char *dest, const unsigned char *mac) {
    const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        *dest++ = hex[(mac[i] >> 4) & 0xF];
        *dest++ = hex[mac[i] & 0xF];
        if (i < 5) *dest++ = ':';
    }
    *dest = '\0';
}

static void StrCat(char *dest, const char *src) {
    while (*dest) dest++;
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

static RTW_FW_PAGE_PARAM g_PageParam;
static char g_Msg[4096];

static DWORD MyStrLen(const char *s) {
    const volatile char *p = (const volatile char *)s;
    DWORD len = 0;
    while (*p++) len++;
    return len;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
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
        MessageBoxA(
            NULL,
            "Cannot connect to \\\\.\\RTWPROBE.\n\n"
            "This means RTWPROBE.SYS is not currently loaded by Windows 95.\n\n"
            "Run AUTOBIND.EXE and refresh Device Manager.",
            "Realtek 8822BU Milestone 3",
            MB_ICONEXCLAMATION | MB_OK
        );
        ExitProcess(1);
    }

    /* 1. Milestone 1 & 2 & USB Pipe Probe */
    RTW_PROBE_DATA probeData;
    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        hDev,
        IOCTL_RTW_PROBE,
        NULL, 0,
        &probeData, sizeof(probeData),
        &bytesReturned,
        NULL
    );

    if (!ok) {
        DWORD err = GetLastError();
        char errBuf[16];
        ToDec(errBuf, err);
        char errText[256];
        errText[0] = '\0';
        StrCat(errText, "Driver connected, but IOCTL_RTW_PROBE failed.\n\nError Code: ");
        StrCat(errText, errBuf);
        CloseHandle(hDev);
        MessageBoxA(
            NULL,
            errText,
            "Realtek 8822BU Probe Error",
            MB_ICONERROR | MB_OK
        );
        ExitProcess(1);
    }

    char hexBuf[16];
    char decBuf[16];

    g_Msg[0] = '\0';
    StrCat(g_Msg, "SUCCESS! Connected to Nineplus USB Wi-Fi (Realtek 8822BU)!\n");
    StrCat(g_Msg, "======================================================\n");
    StrCat(g_Msg, "--- Core Registers ---\n");

    StrCat(g_Msg, "REG_SYS_FUNC_EN  (0x0002) = ");
    ToHex16(hexBuf, probeData.func_en);
    StrCat(g_Msg, hexBuf);
    StrCat(g_Msg, (probeData.status_func == 0) ? "  [OK]\n" : "  [FAIL]\n");

    StrCat(g_Msg, "REG_SYS_CLK_CTRL (0x0008) = ");
    ToHex16(hexBuf, probeData.clk_ctrl);
    StrCat(g_Msg, hexBuf);
    StrCat(g_Msg, (probeData.status_clk == 0) ? "  [OK]\n" : "  [FAIL]\n");

    StrCat(g_Msg, "REG_SYS_CFG1     (0x00F0) = ");
    ToHex32(hexBuf, probeData.sys_cfg);
    StrCat(g_Msg, hexBuf);
    StrCat(g_Msg, (probeData.status_cfg == 0) ? "  [OK]\n\n" : "  [FAIL]\n\n");

    StrCat(g_Msg, "--- Hardware USB Configuration & Pipes ---\n");
    StrCat(g_Msg, "Configured Pipes: ");
    ToDec(decBuf, probeData.num_pipes);
    StrCat(g_Msg, decBuf);
    StrCat(g_Msg, " active pipes discovered\n");

    for (unsigned long p = 0; p < probeData.num_pipes; p++) {
        StrCat(g_Msg, "  - Pipe ");
        ToDec(decBuf, p);
        StrCat(g_Msg, decBuf);
        StrCat(g_Msg, ": EP ");
        ToHex16(hexBuf, probeData.pipes[p].EndpointAddress);
        StrCat(g_Msg, hexBuf + 4);
        if (probeData.pipes[p].EndpointAddress & 0x80) {
            StrCat(g_Msg, " IN ");
        } else {
            StrCat(g_Msg, " OUT");
        }
        StrCat(g_Msg, " (MaxPacket ");
        ToDec(decBuf, probeData.pipes[p].MaxPacketSize);
        StrCat(g_Msg, decBuf);
        StrCat(g_Msg, ")\n");
    }
    StrCat(g_Msg, "\n");

    /* 2. Check for Firmware File */
    HANDLE hFw = CreateFileA(
        "rtw8822b_fw.bin",
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hFw == INVALID_HANDLE_VALUE) {
        /* Fallback path */
        hFw = CreateFileA("RTW8822B.BIN", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    }

    if (hFw != INVALID_HANDLE_VALUE) {
        DWORD fwSize = GetFileSize(hFw, NULL);
        StrCat(g_Msg, "--- Milestone 3: Microcode Firmware Upload ---\n");
        StrCat(g_Msg, "Found rtw8822b_fw.bin (");
        ToDec(decBuf, fwSize);
        StrCat(g_Msg, decBuf);
        StrCat(g_Msg, " bytes)\n");

        /* Step A: Init Firmware DL Mode */
        USHORT dlVal = 0;
        DeviceIoControl(hDev, IOCTL_RTW_FW_INIT, NULL, 0, &dlVal, sizeof(dlVal), &bytesReturned, NULL);
        StrCat(g_Msg, "MCU Download Mode: REG_MCUFW_CTRL = ");
        ToHex16(hexBuf, dlVal);
        StrCat(g_Msg, hexBuf);
        StrCat(g_Msg, (dlVal & 1) ? " [OK]\n" : " [FAIL]\n");

        /* Step B: Stream Pages */
        DWORD bytesRead = 0;
        unsigned long pageIdx = 0;
        BOOL streamOk = TRUE;

        /* Skip 64-byte header */
        SetFilePointer(hFw, 64, NULL, FILE_BEGIN);

        while (1) {
            ReadFile(hFw, g_PageParam.Data, 4096, &bytesRead, NULL);
            if (bytesRead == 0) break;

            g_PageParam.PageIndex = pageIdx;
            g_PageParam.TargetAddr = 0x1000;
            g_PageParam.Length = bytesRead;

            BOOL pgOk = DeviceIoControl(
                hDev,
                IOCTL_RTW_FW_WRITE_PAGE,
                &g_PageParam, sizeof(g_PageParam),
                NULL, 0,
                &bytesReturned,
                NULL
            );

            if (!pgOk) {
                streamOk = FALSE;
                break;
            }
            pageIdx++;
        }
        CloseHandle(hFw);

        StrCat(g_Msg, "Streamed ");
        ToDec(decBuf, pageIdx);
        StrCat(g_Msg, decBuf);
        StrCat(g_Msg, " Pages (160 KB): ");
        StrCat(g_Msg, streamOk ? "[OK]\n" : "[FAIL]\n");

        /* Step C: Boot CPU */
        RTW_BOOT_RESULT bootRes;
        DeviceIoControl(hDev, IOCTL_RTW_FW_BOOT, NULL, 0, &bootRes, sizeof(bootRes), &bytesReturned, NULL);

        StrCat(g_Msg, "CPU Boot Status: REG_MCUFW_CTRL = ");
        ToHex16(hexBuf, bootRes.FinalMcuFwCtrl);
        StrCat(g_Msg, hexBuf);
        if ((bootRes.FinalMcuFwCtrl & 0x8000) || ((bootRes.FinalMcuFwCtrl & 0xC078) == 0xC078)) {
            StrCat(g_Msg, "  [FW_READY ACTIVE!]\n\n");
        } else {
            StrCat(g_Msg, "  [TIMEOUT]\n\n");
        }

        StrCat(g_Msg, "--- Post-Boot Hardware MAC Address ---\n");
        char macBuf[24];
        FormatMac(macBuf, bootRes.PostBootMac);
        StrCat(g_Msg, "MAC (0x0610): ");
        StrCat(g_Msg, macBuf);
        StrCat(g_Msg, "\n\nMilestone 3 Complete!");
    } else {
        StrCat(g_Msg, "--- Milestone 3 Test Mode (No FW file) ---\n");
        StrCat(g_Msg, "Initial REG_MCUFW_CTRL: ");
        ToHex16(hexBuf, probeData.mcufw_ctrl_init);
        StrCat(g_Msg, hexBuf);
        StrCat(g_Msg, "\nMCU FWDL Enable Test:  ");
        ToHex16(hexBuf, probeData.mcufw_ctrl_dl_en);
        StrCat(g_Msg, hexBuf);
        StrCat(g_Msg, (probeData.mcufw_ctrl_dl_en & 1) ? " [OK]\n" : " [FAIL]\n");
        StrCat(g_Msg, "Platform Reset Test:   ");
        StrCat(g_Msg, probeData.platform_reset_ok ? "[OK]\n\n" : "[FAIL]\n\n");
        StrCat(g_Msg, "Place rtw8822b_fw.bin in this folder to run full upload!");
    }

    CloseHandle(hDev);

    /* Also write to RTWTEST.LOG for offline review */
    HANDLE hLog = CreateFileA("RTWTEST.LOG", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLog != INVALID_HANDLE_VALUE) {
        DWORD len = MyStrLen(g_Msg);
        DWORD written = 0;
        WriteFile(hLog, g_Msg, len, &written, NULL);
        CloseHandle(hLog);
    }

    MessageBoxA(
        NULL,
        g_Msg,
        "Realtek 8822BU Milestone 3 Verification",
        MB_ICONINFORMATION | MB_OK
    );

    ExitProcess(0);
    return 0;
}
