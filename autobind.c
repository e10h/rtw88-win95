/*
 * autobind.c - Realtek RTL8822BU Windows 95 Auto-Binder (Milestone 6)
 *
 * Modes:
 *   [Yes] = DUAL MODE (Recommended for Wi-Fi + Web Browsing):
 *           - Installs RTW88.VXD on ROOT\NET\RTW88 (permanent NDIS 3.1 TCP/IP adapter)
 *           - Installs RTWPROBE.SYS on USB\VID_0BDA&PID_B812 (USB Wi-Fi radio access)
 *           Both run simultaneously with zero conflicts and crash-free USB unplug!
 *   [No]  = USB-ONLY NDIS MODE (RTW88.VXD directly on USB\VID_0BDA&PID_B812)
 */
#include <windows.h>

static void StrCat(char *dest, const char *src) {
    while (*dest) dest++;
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

static void StrCpy(char *dest, const char *src) {
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

static int StrEqNoCase(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void FmtIndex(char *buf, int i) {
    buf[0] = '0';
    buf[1] = '0';
    buf[2] = (char)('0' + (i / 10));
    buf[3] = (char)('0' + (i % 10));
    buf[4] = '\0';
}

static void DeleteKeyTree(HKEY hParent, const char *subkeyName) {
    HKEY hKey;
    if (RegOpenKeyExA(hParent, subkeyName, 0, KEY_ALL_ACCESS, &hKey) != ERROR_SUCCESS)
        return;

    char childName[256];
    DWORD childLen;
    for (;;) {
        childLen = sizeof(childName);
        if (RegEnumKeyExA(hKey, 0, childName, &childLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        DeleteKeyTree(hKey, childName);
    }
    RegCloseKey(hKey);
    RegDeleteKeyA(hParent, subkeyName);
}

static void BuildExeDirFilePath(char *outBuf, const char *fileName) {
    char exePath[260];
    DWORD len = GetModuleFileNameA(NULL, exePath, sizeof(exePath) - 1);
    if (len > 0 && len < sizeof(exePath)) {
        exePath[len] = '\0';
        int lastSlash = -1;
        int i;
        for (i = 0; exePath[i]; i++) {
            if (exePath[i] == '\\' || exePath[i] == '/') lastSlash = i;
        }
        if (lastSlash >= 0) {
            exePath[lastSlash + 1] = '\0';
            StrCpy(outBuf, exePath);
            StrCat(outBuf, fileName);
            return;
        }
    }
    StrCpy(outBuf, fileName);
}

static BOOL InstallDriverFile(const char *sysDir, const char *driverName, char *msg) {
    char srcPath[260], destPath[260];
    BuildExeDirFilePath(srcPath, driverName);
    StrCpy(destPath, sysDir);
    StrCat(destPath, "\\");
    StrCat(destPath, driverName);

    BOOL ok = CopyFileA(srcPath, destPath, FALSE);
    if (!ok) ok = CopyFileA(driverName, destPath, FALSE);
    if (ok) {
        StrCat(msg, "  Installed: ");
        StrCat(msg, destPath);
        StrCat(msg, "\n");
        return TRUE;
    }
    StrCat(msg, "  ERROR: Missing ");
    StrCat(msg, driverName);
    StrCat(msg, " next to AUTOBIND.EXE!\n");
    return FALSE;
}

/* Clean stale entries matching either matchId1 or matchId2 in Services\Class\<cls>,
   then allocate and return the first free slot path (e.g. "Net\0001") and open HKEY. */
static HKEY AllocCleanClassSlot(const char *targetClass, const char *matchId1, const char *matchId2,
                                char *outClassPath, char *msg) {
    char classRegPath[128];
    StrCpy(classRegPath, "System\\CurrentControlSet\\Services\\Class\\");
    StrCat(classRegPath, targetClass);

    HKEY hClassKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, classRegPath, 0, KEY_ALL_ACCESS, &hClassKey) != ERROR_SUCCESS) {
        return NULL;
    }

    /* Clean stale keys */
    int i;
    for (i = 0; i <= 99; i++) {
        char subName[8], valBuf[128];
        DWORD valLen = sizeof(valBuf), valType = 0;
        FmtIndex(subName, i);
        HKEY hSub;
        if (RegOpenKeyExA(hClassKey, subName, 0, KEY_READ, &hSub) != ERROR_SUCCESS)
            continue;
        valBuf[0] = '\0';
        RegQueryValueExA(hSub, "MatchingDeviceId", NULL, &valType, (LPBYTE)valBuf, &valLen);
        RegCloseKey(hSub);

        if ((matchId1 && StrEqNoCase(valBuf, matchId1)) ||
            (matchId2 && StrEqNoCase(valBuf, matchId2))) {
            DeleteKeyTree(hClassKey, subName);
            StrCat(msg, "  Cleaned stale ");
            StrCat(msg, targetClass); StrCat(msg, "\\"); StrCat(msg, subName);
            StrCat(msg, "\n");
        }
    }

    /* Find first free slot */
    HKEY hTarget = NULL;
    for (i = 0; i <= 99; i++) {
        char subName[8];
        FmtIndex(subName, i);
        HKEY hTest;
        if (RegOpenKeyExA(hClassKey, subName, 0, KEY_READ, &hTest) == ERROR_SUCCESS) {
            RegCloseKey(hTest);
            continue;
        }
        DWORD disp = 0;
        if (RegCreateKeyExA(hClassKey, subName, 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_ALL_ACCESS, NULL, &hTarget, &disp) == ERROR_SUCCESS) {
            StrCpy(outClassPath, targetClass);
            StrCat(outClassPath, "\\");
            StrCat(outClassPath, subName);
            break;
        }
    }

    RegCloseKey(hClassKey);
    return hTarget;
}

static void PopulateNetClassKey(HKEY hKey, const char *matchId, const char *driverDesc) {
    const char *devLoader = "*ndis";
    const char *vxdName   = "RTW88.VXD";
    const char *propPages = "netdi.dll,EnumPropPages";

    RegSetValueExA(hKey, "DevLoader", 0, REG_SZ, (const BYTE *)devLoader, lstrlenA(devLoader) + 1);
    RegSetValueExA(hKey, "DeviceVxDs", 0, REG_SZ, (const BYTE *)vxdName, lstrlenA(vxdName) + 1);
    RegSetValueExA(hKey, "DriverDesc", 0, REG_SZ, (const BYTE *)driverDesc, lstrlenA(driverDesc) + 1);
    RegSetValueExA(hKey, "MatchingDeviceId", 0, REG_SZ, (const BYTE *)matchId, lstrlenA(matchId) + 1);
    RegSetValueExA(hKey, "EnumPropPages", 0, REG_SZ, (const BYTE *)propPages, lstrlenA(propPages) + 1);
    RegDeleteValueA(hKey, "NTMPDriver");

    HKEY hNdis;
    if (RegCreateKeyExA(hKey, "NDIS", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hNdis, NULL) == ERROR_SUCCESS) {
        const char *logName = "RTW88";
        BYTE majorVer = 3;
        BYTE minorVer = 10;
        RegSetValueExA(hNdis, "LogDriverName", 0, REG_SZ, (const BYTE *)logName, lstrlenA(logName) + 1);
        RegSetValueExA(hNdis, "MajorNdisVersion", 0, REG_BINARY, &majorVer, 1);
        RegSetValueExA(hNdis, "MinorNdisVersion", 0, REG_BINARY, &minorVer, 1);
        RegCloseKey(hNdis);
    }

    HKEY hNdi;
    if (RegCreateKeyExA(hKey, "Ndi", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hNdi, NULL) == ERROR_SUCCESS) {
        const char *cardType = "PNP";
        RegSetValueExA(hNdi, "DeviceID", 0, REG_SZ, (const BYTE *)matchId, lstrlenA(matchId) + 1);
        RegSetValueExA(hNdi, "CardType", 0, REG_SZ, (const BYTE *)cardType, lstrlenA(cardType) + 1);
        RegCloseKey(hNdi);
    }

    HKEY hNdiIf;
    if (RegCreateKeyExA(hKey, "Ndi\\Interfaces", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hNdiIf, NULL) == ERROR_SUCCESS) {
        const char *ndis3 = "ndis3";
        const char *eth   = "ethernet";
        RegSetValueExA(hNdiIf, "DefUpper",   0, REG_SZ, (const BYTE *)ndis3, lstrlenA(ndis3) + 1);
        RegSetValueExA(hNdiIf, "DefLower",   0, REG_SZ, (const BYTE *)eth,   lstrlenA(eth)   + 1);
        RegSetValueExA(hNdiIf, "UpperRange", 0, REG_SZ, (const BYTE *)ndis3, lstrlenA(ndis3) + 1);
        RegSetValueExA(hNdiIf, "LowerRange", 0, REG_SZ, (const BYTE *)eth,   lstrlenA(eth)   + 1);
        RegCloseKey(hNdiIf);
    }
}

static void PopulateUsbClassKey(HKEY hKey, const char *matchId, const char *driverDesc) {
    const char *devLoader = "*ntkern";
    const char *sysName   = "RTWPROBE.SYS";

    RegSetValueExA(hKey, "DevLoader", 0, REG_SZ, (const BYTE *)devLoader, lstrlenA(devLoader) + 1);
    RegSetValueExA(hKey, "NTMPDriver", 0, REG_SZ, (const BYTE *)sysName, lstrlenA(sysName) + 1);
    RegSetValueExA(hKey, "DriverDesc", 0, REG_SZ, (const BYTE *)driverDesc, lstrlenA(driverDesc) + 1);
    RegSetValueExA(hKey, "MatchingDeviceId", 0, REG_SZ, (const BYTE *)matchId, lstrlenA(matchId) + 1);
    RegDeleteValueA(hKey, "DeviceVxDs");
}

static int BindUsbInstances(const char *cls, const char *guid, const char *desc,
                            const char *classPath, const char *devLoader,
                            const char *drvValName, const char *drvVal, const char *delValName,
                            char *msg) {
    HKEY hEnumVid;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Enum\\USB\\VID_0BDA&PID_B812",
                      0, KEY_READ | KEY_WRITE, &hEnumVid) != ERROR_SUCCESS) {
        return 0;
    }

    char instName[256];
    DWORD instLen = sizeof(instName);
    DWORD idx = 0;
    int count = 0;

    while (RegEnumKeyExA(hEnumVid, idx, instName, &instLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        HKEY hInst;
        if (RegOpenKeyExA(hEnumVid, instName, 0, KEY_ALL_ACCESS, &hInst) == ERROR_SUCCESS) {
            DWORD cfgFlags = 0;
            RegSetValueExA(hInst, "Class", 0, REG_SZ, (const BYTE *)cls, lstrlenA(cls) + 1);
            RegSetValueExA(hInst, "ClassGUID", 0, REG_SZ, (const BYTE *)guid, lstrlenA(guid) + 1);
            RegSetValueExA(hInst, "DeviceDesc", 0, REG_SZ, (const BYTE *)desc, lstrlenA(desc) + 1);
            RegSetValueExA(hInst, "Driver", 0, REG_SZ, (const BYTE *)classPath, lstrlenA(classPath) + 1);
            RegSetValueExA(hInst, "DevLoader", 0, REG_SZ, (const BYTE *)devLoader, lstrlenA(devLoader) + 1);
            RegSetValueExA(hInst, "ConfigFlags", 0, REG_BINARY, (const BYTE *)&cfgFlags, 4);
            RegSetValueExA(hInst, drvValName, 0, REG_SZ, (const BYTE *)drvVal, lstrlenA(drvVal) + 1);
            RegDeleteValueA(hInst, delValName);
            RegCloseKey(hInst);
            count++;
            StrCat(msg, "  Bound USB: "); StrCat(msg, instName); StrCat(msg, "\n");
        }
        idx++;
        instLen = sizeof(instName);
    }

    RegCloseKey(hEnumVid);
    return count;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    char msg[2048];
    msg[0] = '\0';

    (void)hInst; (void)hPrev; (void)lpCmdLine; (void)nCmdShow;

    int choice = MessageBoxA(
        NULL,
        "=== Realtek RTL8822BU Windows 95 Auto-Binder (Milestone 6) ===\n\n"
        "Choose Driver Mode:\n\n"
        "[Yes]    = DUAL MODE (Recommended for Wi-Fi + Web!)\n"
        "           - ROOT\\NET\\RTW88 -> RTW88.VXD (NDIS 3.1 TCP/IP)\n"
        "           - USB Dongle      -> RTWPROBE.SYS (USB Wi-Fi Radio)\n\n"
        "[No]     = USB-ONLY NDIS MODE (RTW88.VXD directly on USB)\n"
        "[Cancel] = Exit without changes",
        "Select Driver Mode",
        MB_YESNOCANCEL | MB_ICONQUESTION
    );

    if (choice == IDCANCEL) {
        ExitProcess(0);
    }

    BOOL isDualMode = (choice == IDYES);

    char sysDir[260];
    GetSystemDirectoryA(sysDir, sizeof(sysDir));

    const char *netDesc  = "Realtek RTL8822BU Wireless LAN 802.11ac Adapter";
    const char *usbDesc  = "Realtek 8822BU USB Wi-Fi Radio (RTWPROBE)";
    const char *netGuid  = "{4d36e972-e325-11ce-bfc1-08002be10318}";
    const char *usbGuid  = "{36fc9e60-c465-11cf-8056-444553540000}";
    const char *usbId    = "USB\\VID_0BDA&PID_B812";
    const char *rootId   = "RTW88_NET";

    if (isDualMode) {
        StrCat(msg, "=== DUAL MODE: RTW88.VXD (Net) + RTWPROBE.SYS (USB) ===\n\n");

        if (!InstallDriverFile(sysDir, "RTW88.VXD", msg) ||
            !InstallDriverFile(sysDir, "RTWPROBE.SYS", msg)) {
            MessageBoxA(NULL, msg, "Auto-Binder Error", MB_ICONERROR);
            ExitProcess(5);
        }

        /* 1. Create Services\Class\Net\000x for RTW88.VXD */
        char netClassPath[32];
        HKEY hNetCls = AllocCleanClassSlot("Net", usbId, rootId, netClassPath, msg);
        if (!hNetCls) {
            MessageBoxA(NULL, "Failed to allocate Services\\Class\\Net slot!", "Auto-Binder Error", MB_ICONERROR);
            ExitProcess(1);
        }
        PopulateNetClassKey(hNetCls, rootId, netDesc);
        RegCloseKey(hNetCls);
        StrCat(msg, "1. Created Net Class: "); StrCat(msg, netClassPath); StrCat(msg, "\n");

        /* 2. Create HKLM\Enum\Root\Net\RTW88 pointing to Net\000x */
        HKEY hRootNet;
        if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Enum\\Root\\Net\\RTW88", 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hRootNet, NULL) == ERROR_SUCCESS) {
            DWORD cfgFlags = 0;
            const char *clsNet = "Net";
            const char *mfg    = "Realtek Semiconductor Corp.";
            const char *devLdr = "*ndis";
            const char *vxd    = "RTW88.VXD";
            RegSetValueExA(hRootNet, "Class", 0, REG_SZ, (const BYTE *)clsNet, lstrlenA(clsNet) + 1);
            RegSetValueExA(hRootNet, "ClassGUID", 0, REG_SZ, (const BYTE *)netGuid, lstrlenA(netGuid) + 1);
            RegSetValueExA(hRootNet, "CompatibleIDs", 0, REG_SZ, (const BYTE *)rootId, lstrlenA(rootId) + 1);
            RegSetValueExA(hRootNet, "HardwareID", 0, REG_SZ, (const BYTE *)rootId, lstrlenA(rootId) + 1);
            RegSetValueExA(hRootNet, "DeviceDesc", 0, REG_SZ, (const BYTE *)netDesc, lstrlenA(netDesc) + 1);
            RegSetValueExA(hRootNet, "Mfg", 0, REG_SZ, (const BYTE *)mfg, lstrlenA(mfg) + 1);
            RegSetValueExA(hRootNet, "Driver", 0, REG_SZ, (const BYTE *)netClassPath, lstrlenA(netClassPath) + 1);
            RegSetValueExA(hRootNet, "DevLoader", 0, REG_SZ, (const BYTE *)devLdr, lstrlenA(devLdr) + 1);
            RegSetValueExA(hRootNet, "DeviceVxDs", 0, REG_SZ, (const BYTE *)vxd, lstrlenA(vxd) + 1);
            RegSetValueExA(hRootNet, "ConfigFlags", 0, REG_BINARY, (const BYTE *)&cfgFlags, 4);
            RegDeleteValueA(hRootNet, "NTMPDriver");
            RegCloseKey(hRootNet);
            StrCat(msg, "2. Created Enum\\Root\\Net\\RTW88 -> "); StrCat(msg, netClassPath); StrCat(msg, "\n");
        }

        /* 3. Create Services\Class\USB\000x for RTWPROBE.SYS */
        char usbClassPath[32];
        HKEY hUsbCls = AllocCleanClassSlot("USB", usbId, NULL, usbClassPath, msg);
        if (!hUsbCls) {
            MessageBoxA(NULL, "Failed to allocate Services\\Class\\USB slot!", "Auto-Binder Error", MB_ICONERROR);
            ExitProcess(2);
        }
        PopulateUsbClassKey(hUsbCls, usbId, usbDesc);
        RegCloseKey(hUsbCls);
        StrCat(msg, "3. Created USB Class: "); StrCat(msg, usbClassPath); StrCat(msg, "\n");

        /* 4. Bind USB\VID_0BDA&PID_B812 to RTWPROBE.SYS */
        int cnt = BindUsbInstances("USB", usbGuid, usbDesc, usbClassPath,
                                   "*ntkern", "NTMPDriver", "RTWPROBE.SYS", "DeviceVxDs", msg);
        if (cnt == 0) {
            StrCat(msg, "  (Note: USB dongle not plugged in yet; plug in & re-run if needed.)\n");
        }

        StrCat(msg, "\nSUCCESS! Dual Mode is ready.\n\n"
                    "Next Steps:\n"
                    "1. REBOOT Windows 95.\n"
                    "2. Check Control Panel -> Network -> verify TCP/IP is bound to Realtek\n"
                    "   (set static IP 192.168.1.150 / 255.255.255.0 if needed).\n"
                    "3. Run PINGTEST.EXE and test 'ping 192.168.1.1' in MS-DOS Prompt!");
    } else {
        StrCat(msg, "=== USB-ONLY NDIS MODE: RTW88.VXD on USB ===\n\n");

        if (!InstallDriverFile(sysDir, "RTW88.VXD", msg)) {
            MessageBoxA(NULL, msg, "Auto-Binder Error", MB_ICONERROR);
            ExitProcess(5);
        }

        /* Remove Root\Net\RTW88 if present */
        HKEY hRoot;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Enum\\Root\\Net", 0, KEY_ALL_ACCESS, &hRoot) == ERROR_SUCCESS) {
            DeleteKeyTree(hRoot, "RTW88");
            RegCloseKey(hRoot);
        }

        char netClassPath[32];
        HKEY hNetCls = AllocCleanClassSlot("Net", usbId, rootId, netClassPath, msg);
        if (!hNetCls) {
            MessageBoxA(NULL, "Failed to allocate Services\\Class\\Net slot!", "Auto-Binder Error", MB_ICONERROR);
            ExitProcess(1);
        }
        PopulateNetClassKey(hNetCls, usbId, netDesc);
        RegCloseKey(hNetCls);
        StrCat(msg, "1. Created Net Class: "); StrCat(msg, netClassPath); StrCat(msg, "\n");

        int cnt = BindUsbInstances("Net", netGuid, netDesc, netClassPath,
                                   "*ndis", "DeviceVxDs", "RTW88.VXD", "NTMPDriver", msg);
        if (cnt == 0) {
            StrCat(msg, "WARNING: USB\\VID_0BDA&PID_B812 not found! Plug in dongle and re-run.\n");
        }
        StrCat(msg, "\nDone! Reboot Windows 95.");
    }

    MessageBoxA(NULL, msg, "Auto-Binder - Success", MB_ICONINFORMATION);
    ExitProcess(0);
}
