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

/* Format integer as 4-digit zero-padded decimal string into buf */
static void FmtIndex(char *buf, int i) {
    buf[0] = '0';
    buf[1] = '0';
    buf[2] = (char)('0' + (i / 10));
    buf[3] = (char)('0' + (i % 10));
    buf[4] = '\0';
}

/* Recursively delete all subkeys of hKey then delete hKey itself from hParent */
static void DeleteKeyTree(HKEY hParent, const char *subkeyName) {
    HKEY hKey;
    if (RegOpenKeyExA(hParent, subkeyName, 0, KEY_ALL_ACCESS, &hKey) != ERROR_SUCCESS)
        return;

    char childName[256];
    DWORD childLen;
    /* Keep deleting the first subkey until none remain */
    for (;;) {
        childLen = sizeof(childName);
        if (RegEnumKeyExA(hKey, 0, childName, &childLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        DeleteKeyTree(hKey, childName);
    }
    RegCloseKey(hKey);
    RegDeleteKeyA(hParent, subkeyName);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    char msg[2048];
    msg[0] = '\0';

    int choice = MessageBoxA(
        NULL,
        "=== Realtek RTL8822BU Windows 95 Auto-Binder ===\n\n"
        "Choose Driver Mode:\n\n"
        "[Yes]    = Bind RTW88.SYS (NDIS 3.1 Network Miniport for TCP/IP)\n"
        "[No]     = Bind RTWPROBE.SYS (WDM Diagnostics & RTWSCAN)\n"
        "[Cancel] = Exit without changes",
        "Select Driver Mode",
        MB_YESNOCANCEL | MB_ICONQUESTION
    );

    if (choice == IDCANCEL) {
        ExitProcess(0);
    }

    BOOL isNetMode = (choice == IDYES);

    const char *targetClass = isNetMode ? "Net" : "USB";
    const char *driverName  = isNetMode ? "RTW88.SYS" : "RTWPROBE.SYS";
    const char *devLoader   = isNetMode ? "*ndis" : "*ntkern";
    const char *driverDesc  = isNetMode
        ? "Realtek RTL8822BU Wireless LAN 802.11ac Adapter"
        : "Realtek 8822BU USB Wi-Fi Probe Driver";
    const char *clsGuid = isNetMode
        ? "{4d36e972-e325-11ce-bfc1-08002be10318}"
        : "{36fc9e60-c465-11cf-8056-444553540000}";
    const char *matchId = "USB\\VID_0BDA&PID_B812";

    StrCat(msg, isNetMode
        ? "=== Binding NDIS 3.1 Miniport Driver (RTW88.SYS) ===\n\n"
        : "=== Binding WDM Diagnostic Driver (RTWPROBE.SYS) ===\n\n");

    /* -------------------------------------------------------
       Auto-copy driver file to C:\WINDOWS\SYSTEM
    ------------------------------------------------------- */
    char sysDir[260];
    UINT sysLen = GetSystemDirectoryA(sysDir, sizeof(sysDir));
    if (sysLen > 0 && sysLen < 240) {
        char destPath[260];
        StrCpy(destPath, sysDir);
        StrCat(destPath, "\\");
        StrCat(destPath, driverName);
        if (CopyFileA(driverName, destPath, FALSE)) {
            StrCat(msg, "0. Installed driver to: ");
            StrCat(msg, destPath);
            StrCat(msg, "\n");
        } else {
            StrCat(msg, "0. WARNING: CopyFile failed (is driver in same dir as AUTOBIND.EXE?)\n");
        }
    }

    /* -------------------------------------------------------
       1. Open Services\Class\<targetClass>
    ------------------------------------------------------- */
    char classRegPath[128];
    classRegPath[0] = '\0';
    StrCat(classRegPath, "System\\CurrentControlSet\\Services\\Class\\");
    StrCat(classRegPath, targetClass);

    HKEY hClassKey;
    LONG res = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        classRegPath,
        0, KEY_ALL_ACCESS, &hClassKey
    );

    if (res != ERROR_SUCCESS) {
        MessageBoxA(NULL, "Failed to open Services\\Class registry key!", "Auto-Binder Error", MB_ICONERROR);
        ExitProcess(1);
    }

    /* -------------------------------------------------------
       2. CLEAN SLATE: Delete all existing stale Realtek entries
          Scan 0000-0099, delete any with MatchingDeviceId = our HW ID
    ------------------------------------------------------- */
    {
        char valBuf[128];
        DWORD valLen;
        DWORD valType;
        int i;
        for (i = 0; i <= 99; i++) {
            char subName[8];
            FmtIndex(subName, i);

            HKEY hSub;
            if (RegOpenKeyExA(hClassKey, subName, 0, KEY_READ, &hSub) != ERROR_SUCCESS)
                continue;

            valLen = sizeof(valBuf);
            valBuf[0] = '\0';
            RegQueryValueExA(hSub, "MatchingDeviceId", NULL, &valType, (LPBYTE)valBuf, &valLen);
            RegCloseKey(hSub);

            if (StrEqNoCase(valBuf, matchId)) {
                /* Delete this stale key and all its subkeys */
                DeleteKeyTree(hClassKey, subName);
                StrCat(msg, "  Cleaned stale key: ");
                StrCat(msg, targetClass);
                StrCat(msg, "\\");
                StrCat(msg, subName);
                StrCat(msg, "\n");
            }
        }
    }

    /* -------------------------------------------------------
       3. Find the first free slot 0000-0099 and create it
    ------------------------------------------------------- */
    char chosenClass[8];
    char chosenClassPath[32];
    chosenClass[0] = '\0';
    chosenClassPath[0] = '\0';

    HKEY hTargetClass = NULL;
    {
        int i;
        for (i = 0; i <= 99; i++) {
            char subName[8];
            FmtIndex(subName, i);

            /* Try to open — if it doesn't exist, that slot is free */
            HKEY hTest;
            LONG openRes = RegOpenKeyExA(hClassKey, subName, 0, KEY_READ, &hTest);
            if (openRes == ERROR_SUCCESS) {
                RegCloseKey(hTest);
                continue; /* Slot occupied by something else, skip */
            }

            /* Slot is free — create it */
            DWORD disp = 0;
            res = RegCreateKeyExA(
                hClassKey, subName, 0, NULL,
                REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL,
                &hTargetClass, &disp
            );

            if (res == ERROR_SUCCESS) {
                StrCpy(chosenClass, subName);
                StrCpy(chosenClassPath, targetClass);
                StrCat(chosenClassPath, "\\");
                StrCat(chosenClassPath, subName);
                break;
            }
        }
    }

    if (!hTargetClass) {
        RegCloseKey(hClassKey);
        MessageBoxA(NULL, "Failed to create Services\\Class target key!", "Auto-Binder Error", MB_ICONERROR);
        ExitProcess(2);
    }

    /* -------------------------------------------------------
       4. Populate the Class key
    ------------------------------------------------------- */
    RegSetValueExA(hTargetClass, "DevLoader", 0, REG_SZ,
        (const BYTE *)devLoader, lstrlenA(devLoader) + 1);
    RegSetValueExA(hTargetClass, "DriverDesc", 0, REG_SZ,
        (const BYTE *)driverDesc, lstrlenA(driverDesc) + 1);
    RegSetValueExA(hTargetClass, "MatchingDeviceId", 0, REG_SZ,
        (const BYTE *)matchId, lstrlenA(matchId) + 1);

    if (isNetMode) {
        RegSetValueExA(hTargetClass, "DeviceVxDs", 0, REG_SZ,
            (const BYTE *)driverName, lstrlenA(driverName) + 1);
        /* Explicitly ensure NTMPDriver does NOT exist */
        RegDeleteValueA(hTargetClass, "NTMPDriver");

        /* NDIS Subkey */
        HKEY hNdis;
        if (RegCreateKeyExA(hTargetClass, "NDIS", 0, NULL, 0, KEY_ALL_ACCESS,
                            NULL, &hNdis, NULL) == ERROR_SUCCESS) {
            const char *logName = "RTW88";
            BYTE majorVer = 3;
            BYTE minorVer = 10;
            RegSetValueExA(hNdis, "LogDriverName", 0, REG_SZ,
                (const BYTE *)logName, lstrlenA(logName) + 1);
            RegSetValueExA(hNdis, "MajorNdisVersion", 0, REG_BINARY, &majorVer, 1);
            RegSetValueExA(hNdis, "MinorNdisVersion", 0, REG_BINARY, &minorVer, 1);
            RegCloseKey(hNdis);
        }

        /* Ndi\Interfaces Subkey */
        HKEY hNdiIf;
        if (RegCreateKeyExA(hTargetClass, "Ndi\\Interfaces", 0, NULL, 0, KEY_ALL_ACCESS,
                            NULL, &hNdiIf, NULL) == ERROR_SUCCESS) {
            const char *ndis3   = "ndis3";
            const char *eth     = "ethernet";
            RegSetValueExA(hNdiIf, "DefUpper",   0, REG_SZ, (const BYTE *)ndis3, lstrlenA(ndis3) + 1);
            RegSetValueExA(hNdiIf, "DefLower",   0, REG_SZ, (const BYTE *)eth,   lstrlenA(eth)   + 1);
            RegSetValueExA(hNdiIf, "UpperRange",  0, REG_SZ, (const BYTE *)ndis3, lstrlenA(ndis3) + 1);
            RegSetValueExA(hNdiIf, "LowerRange",  0, REG_SZ, (const BYTE *)eth,   lstrlenA(eth)   + 1);
            RegCloseKey(hNdiIf);
        }
    } else {
        RegSetValueExA(hTargetClass, "NTMPDriver", 0, REG_SZ,
            (const BYTE *)driverName, lstrlenA(driverName) + 1);
        RegDeleteValueA(hTargetClass, "DeviceVxDs");
    }

    RegCloseKey(hTargetClass);
    RegCloseKey(hClassKey);

    StrCat(msg, "1. Created Driver Class: ");
    StrCat(msg, chosenClassPath);
    StrCat(msg, "\n");

    /* -------------------------------------------------------
       5. Enumerate Enum\USB\VID_0BDA&PID_B812 and bind every instance
    ------------------------------------------------------- */
    HKEY hEnumVid;
    res = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "Enum\\USB\\VID_0BDA&PID_B812",
        0, KEY_READ | KEY_WRITE, &hEnumVid
    );

    if (res != ERROR_SUCCESS) {
        StrCat(msg, "\nERROR: Enum\\USB\\VID_0BDA&PID_B812 was NOT found!\n\n"
                    "The Wi-Fi dongle is not plugged in or not detected by USB.\n\n"
                    "Plug the dongle in, wait 2 seconds, then run Auto-Binder again.");
        MessageBoxA(NULL, msg, "Auto-Binder Warning", MB_ICONEXCLAMATION);
        ExitProcess(3);
    }

    char instName[256];
    DWORD instLen = sizeof(instName);
    DWORD enumIdx = 0;
    int boundCount = 0;

    while (RegEnumKeyExA(hEnumVid, enumIdx, instName, &instLen,
                         NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        HKEY hInstKey;
        res = RegOpenKeyExA(hEnumVid, instName, 0, KEY_ALL_ACCESS, &hInstKey);
        if (res == ERROR_SUCCESS) {
            DWORD cfgFlags = 0;

            RegSetValueExA(hInstKey, "Class", 0, REG_SZ,
                (const BYTE *)targetClass, lstrlenA(targetClass) + 1);
            RegSetValueExA(hInstKey, "ClassGUID", 0, REG_SZ,
                (const BYTE *)clsGuid, lstrlenA(clsGuid) + 1);
            RegSetValueExA(hInstKey, "DeviceDesc", 0, REG_SZ,
                (const BYTE *)driverDesc, lstrlenA(driverDesc) + 1);
            RegSetValueExA(hInstKey, "Driver", 0, REG_SZ,
                (const BYTE *)chosenClassPath, lstrlenA(chosenClassPath) + 1);
            RegSetValueExA(hInstKey, "DevLoader", 0, REG_SZ,
                (const BYTE *)devLoader, lstrlenA(devLoader) + 1);
            /* ConfigFlags = 0 means no problem flags */
            RegSetValueExA(hInstKey, "ConfigFlags", 0, REG_BINARY,
                (const BYTE *)&cfgFlags, 4);

            if (isNetMode) {
                RegSetValueExA(hInstKey, "DeviceVxDs", 0, REG_SZ,
                    (const BYTE *)driverName, lstrlenA(driverName) + 1);
                RegDeleteValueA(hInstKey, "NTMPDriver");
            } else {
                RegSetValueExA(hInstKey, "NTMPDriver", 0, REG_SZ,
                    (const BYTE *)driverName, lstrlenA(driverName) + 1);
                RegDeleteValueA(hInstKey, "DeviceVxDs");
            }

            RegCloseKey(hInstKey);
            boundCount++;

            StrCat(msg, "2. Bound Hardware Instance: ");
            StrCat(msg, instName);
            StrCat(msg, "\n");
        }
        enumIdx++;
        instLen = sizeof(instName);
    }

    RegCloseKey(hEnumVid);

    if (boundCount == 0) {
        StrCat(msg, "\nNo sub-instances found under VID_0BDA&PID_B812!");
        MessageBoxA(NULL, msg, "Auto-Binder Warning", MB_ICONEXCLAMATION);
        ExitProcess(4);
    }

    StrCat(msg, "\nSUCCESS! Registry is now bound to ");
    StrCat(msg, driverName);
    StrCat(msg, "!\n\n"
                "Next Steps:\n"
                "1. Close this dialog.\n"
                "2. REBOOT or unplug/replug the Wi-Fi dongle.\n"
                "3. Device Manager -> Refresh.\n");
    if (isNetMode) {
        StrCat(msg, "4. Realtek adapter should appear under 'Network adapters' with no error.\n"
                    "5. Run winipcfg to verify IP/MAC assignment.");
    } else {
        StrCat(msg, "4. Run RTWTEST.EXE or RTWSCAN.EXE.");
    }

    MessageBoxA(NULL, msg, "Auto-Binder - Success", MB_ICONINFORMATION);
    ExitProcess(0);
}
