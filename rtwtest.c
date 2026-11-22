#include <windows.h>

#define IOCTL_RTW_PROBE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

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
} RTW_PROBE_DATA;

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
            "Check Device Manager to ensure the device has 'Realtek 8822BU' listed\n"
            "rather than 'Unknown Device'.",
            "Realtek 8822BU Probe Tool",
            MB_ICONEXCLAMATION | MB_OK
        );
        ExitProcess(1);
    }

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

    CloseHandle(hDev);

    if (!ok || bytesReturned < sizeof(probeData)) {
        MessageBoxA(
            NULL,
            "Driver connected, but DeviceIoControl returned an error.",
            "Realtek 8822BU Probe Tool",
            MB_ICONERROR | MB_OK
        );
        ExitProcess(2);
    }

    char msg[512];
    char hexBuf[32];
    msg[0] = '\0';

    StrCat(msg, "SUCCESS! Connected to Nineplus USB Wi-Fi (Realtek 8822BU)!\n\n");
    StrCat(msg, "--- Core Registers ---\n");

    StrCat(msg, "REG_SYS_FUNC_EN  (0x0002) = ");
    ToHex16(hexBuf, probeData.func_en);
    StrCat(msg, hexBuf);
    if (probeData.status_func == 0) StrCat(msg, "  [OK]\n");
    else StrCat(msg, "  [FAILED]\n");

    StrCat(msg, "REG_SYS_CLK_CTRL (0x0008) = ");
    ToHex16(hexBuf, probeData.clk_ctrl);
    StrCat(msg, hexBuf);
    if (probeData.status_clk == 0) StrCat(msg, "  [OK]\n");
    else StrCat(msg, "  [FAILED]\n");

    StrCat(msg, "REG_SYS_CFG1     (0x00F0) = ");
    ToHex32(hexBuf, probeData.sys_cfg);
    StrCat(msg, hexBuf);
    if (probeData.status_cfg == 0) StrCat(msg, "  [OK]\n");
    else StrCat(msg, "  [FAILED]\n");

    StrCat(msg, "\n--- Hardware MAC Address ---\n");

    StrCat(msg, "MAC (Port 0 Reg 0x0610):  ");
    FormatMac(hexBuf, probeData.reg_mac_0610);
    StrCat(msg, hexBuf);
    if (probeData.status_mac_0610 == 0) StrCat(msg, "  [OK]\n");
    else StrCat(msg, "  [FAILED]\n");

    StrCat(msg, "MAC (Station Reg 0x0050): ");
    FormatMac(hexBuf, probeData.reg_mac_0050);
    StrCat(msg, hexBuf);
    if (probeData.status_mac_0050 == 0) StrCat(msg, "  [OK]\n");
    else StrCat(msg, "  [FAILED]\n");

    StrCat(msg, "\nMilestone 2 MAC Identification Complete!");

    MessageBoxA(NULL, msg, "Realtek 8822BU Probe Tool - Milestone 2", MB_ICONINFORMATION | MB_OK);
    ExitProcess(0);
}
