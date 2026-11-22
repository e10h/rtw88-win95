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

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    char msg[1024];
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
    const char *driverName = isNetMode ? "RTW88.SYS" : "RTWPROBE.SYS";
    const char *devLoader = isNetMode ? "*ndis,*ntkern" : "*ntkern";
    const char *driverDesc = isNetMode
        ? "Realtek RTL8822BU Wireless LAN 802.11ac Adapter"
        : "Realtek 8822BU USB Wi-Fi Probe Driver";
    const char *clsGuid = isNetMode
        ? "{4d36e972-e325-11ce-bfc1-08002be10318}"
        : "{36fc9e60-c465-11cf-8056-444553540000}";
    const char *matchId = "USB\\VID_0BDA&PID_B812";

    StrCat(msg, isNetMode
        ? "=== Binding NDIS 3.1 Miniport Driver (RTW88.SYS) ===\n\n"
        : "=== Binding WDM Diagnostic Driver (RTWPROBE.SYS) ===\n\n");

    /* Auto-copy driver file to C:\WINDOWS\SYSTEM */
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
        }
    }

    /* 1. Open Services\Class\<targetClass> */
    char classRegPath[128];
    classRegPath[0] = '\0';
    StrCat(classRegPath, "System\\CurrentControlSet\\Services\\Class\\");
    StrCat(classRegPath, targetClass);

    HKEY hClassKey;
    LONG res = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        classRegPath,
        0, KEY_READ | KEY_WRITE, &hClassKey
    );

    if (res != ERROR_SUCCESS) {
        MessageBoxA(NULL, "Failed to open Services\\Class registry key!", "Auto-Binder Error", MB_ICONERROR);
        ExitProcess(1);
    }

    char chosenClass[16];
    chosenClass[0] = '\0';
    char chosenClassPath[32];
    chosenClassPath[0] = '\0';

    /* Find an available index starting from 0005 upwards */
    HKEY hTargetClass = NULL;
    for (int i = 5; i <= 9; i++) {
        char subName[8];
        subName[0] = '0';
        subName[1] = '0';
        subName[2] = '0';
        subName[3] = (char)('0' + i);
        subName[4] = '\0';

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

    if (!hTargetClass) {
        RegCloseKey(hClassKey);
        MessageBoxA(NULL, "Failed to create Services\\Class target key!", "Auto-Binder Error", MB_ICONERROR);
        ExitProcess(2);
    }

    /* Populate the Class key */
    RegSetValueExA(hTargetClass, "DevLoader", 0, REG_SZ, (const BYTE *)devLoader, lstrlenA(devLoader) + 1);
    RegSetValueExA(hTargetClass, "DriverDesc", 0, REG_SZ, (const BYTE *)driverDesc, lstrlenA(driverDesc) + 1);
    RegSetValueExA(hTargetClass, "MatchingDeviceId", 0, REG_SZ, (const BYTE *)matchId, lstrlenA(matchId) + 1);

    if (isNetMode) {
        RegSetValueExA(hTargetClass, "DeviceVxDs", 0, REG_SZ, (const BYTE *)driverName, lstrlenA(driverName) + 1);
        RegSetValueExA(hTargetClass, "NTMPDriver", 0, REG_SZ, (const BYTE *)driverName, lstrlenA(driverName) + 1);

        /* NDIS Specific Subkeys */
        HKEY hNdis;
        if (RegCreateKeyExA(hTargetClass, "NDIS", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hNdis, NULL) == ERROR_SUCCESS) {
            const char *logName = "RTW88";
            BYTE majorVer = 3;
            BYTE minorVer = 10;
            RegSetValueExA(hNdis, "LogDriverName", 0, REG_SZ, (const BYTE *)logName, lstrlenA(logName) + 1);
            RegSetValueExA(hNdis, "MajorNdisVersion", 0, REG_BINARY, &majorVer, 1);
            RegSetValueExA(hNdis, "MinorNdisVersion", 0, REG_BINARY, &minorVer, 1);
            RegCloseKey(hNdis);
        }

        /* Ndi\Interfaces Subkeys */
        HKEY hNdiIf;
        if (RegCreateKeyExA(hTargetClass, "Ndi\\Interfaces", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hNdiIf, NULL) == ERROR_SUCCESS) {
            const char *defUpper = "ndis3";
            const char *defLower = "ethernet";
            RegSetValueExA(hNdiIf, "DefUpper", 0, REG_SZ, (const BYTE *)defUpper, lstrlenA(defUpper) + 1);
            RegSetValueExA(hNdiIf, "DefLower", 0, REG_SZ, (const BYTE *)defLower, lstrlenA(defLower) + 1);
            RegSetValueExA(hNdiIf, "UpperRange", 0, REG_SZ, (const BYTE *)defUpper, lstrlenA(defUpper) + 1);
            RegSetValueExA(hNdiIf, "LowerRange", 0, REG_SZ, (const BYTE *)defLower, lstrlenA(defLower) + 1);
            RegCloseKey(hNdiIf);
        }
    } else {
        RegSetValueExA(hTargetClass, "NTMPDriver", 0, REG_SZ, (const BYTE *)driverName, lstrlenA(driverName) + 1);
    }

    RegCloseKey(hTargetClass);
    RegCloseKey(hClassKey);

    StrCat(msg, "1. Configured Driver Class: ");
    StrCat(msg, chosenClassPath);
    StrCat(msg, "\n");

    /* 2. Enumerate Enum\USB\VID_0BDA&PID_B812 to bind every hardware instance */
    HKEY hEnumVid;
    res = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "Enum\\USB\\VID_0BDA&PID_B812",
        0, KEY_READ | KEY_WRITE, &hEnumVid
    );

    if (res != ERROR_SUCCESS) {
        StrCat(msg, "\nERROR: Enum\\USB\\VID_0BDA&PID_B812 was NOT found!\n\n"
                    "This means the Wi-Fi dongle has not been plugged in yet\n"
                    "or the USB controller did not detect it.\n\n"
                    "Please plug the Wi-Fi dongle in and click Refresh in Device Manager,\n"
                    "then run this Auto-Binder again.");
        MessageBoxA(NULL, msg, "Auto-Binder Warning", MB_ICONEXCLAMATION);
        ExitProcess(3);
    }

    char instName[256];
    DWORD instLen = sizeof(instName);
    DWORD index = 0;
    int boundCount = 0;

    while (RegEnumKeyExA(hEnumVid, index, instName, &instLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        HKEY hInstKey;
        res = RegOpenKeyExA(hEnumVid, instName, 0, KEY_ALL_ACCESS, &hInstKey);
        if (res == ERROR_SUCCESS) {
            DWORD cfgFlags = 0;

            RegSetValueExA(hInstKey, "Class", 0, REG_SZ, (const BYTE *)targetClass, lstrlenA(targetClass) + 1);
            RegSetValueExA(hInstKey, "ClassGUID", 0, REG_SZ, (const BYTE *)clsGuid, lstrlenA(clsGuid) + 1);
            RegSetValueExA(hInstKey, "DeviceDesc", 0, REG_SZ, (const BYTE *)driverDesc, lstrlenA(driverDesc) + 1);
            RegSetValueExA(hInstKey, "Driver", 0, REG_SZ, (const BYTE *)chosenClassPath, lstrlenA(chosenClassPath) + 1);
            RegSetValueExA(hInstKey, "DevLoader", 0, REG_SZ, (const BYTE *)devLoader, lstrlenA(devLoader) + 1);
            RegSetValueExA(hInstKey, "NTMPDriver", 0, REG_SZ, (const BYTE *)driverName, lstrlenA(driverName) + 1);
            if (isNetMode) {
                RegSetValueExA(hInstKey, "DeviceVxDs", 0, REG_SZ, (const BYTE *)driverName, lstrlenA(driverName) + 1);
            }
            RegSetValueExA(hInstKey, "ConfigFlags", 0, REG_BINARY, (const BYTE *)&cfgFlags, 4);

            RegCloseKey(hInstKey);
            boundCount++;

            StrCat(msg, "2. Bound Hardware Instance: ");
            StrCat(msg, instName);
            StrCat(msg, "\n");
        }
        index++;
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
                "Next Step:\n"
                "1. Go to Device Manager and click 'Refresh' (or unplug/replug the Wi-Fi dongle).\n");
    if (isNetMode) {
        StrCat(msg, "2. Open Network Control Panel or Command Prompt to test TCP/IP (e.g. winipcfg or ping)!");
    } else {
        StrCat(msg, "2. Run RTWTEST.EXE or RTWSCAN.EXE!");
    }

    MessageBoxA(NULL, msg, "Auto-Binder - Success", MB_ICONINFORMATION);
    ExitProcess(0);
}
