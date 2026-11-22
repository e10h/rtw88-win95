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

    StrCat(msg, "=== Realtek 8822BU Windows 95 Auto-Binder ===\n\n");

    /* 1. Find or create a safe Services\Class\USB subkey */
    HKEY hClassUsb;
    LONG res = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "System\\CurrentControlSet\\Services\\Class\\USB",
        0, KEY_READ | KEY_WRITE, &hClassUsb
    );

    if (res != ERROR_SUCCESS) {
        MessageBoxA(NULL, "Failed to open Services\\Class\\USB!", "Auto-Binder Error", MB_ICONERROR);
        ExitProcess(1);
    }

    char chosenClass[16];
    chosenClass[0] = '\0';
    char chosenClassPath[32];
    chosenClassPath[0] = '\0';

    /* Look for an index from 0005 upwards so we never conflict with 0000/0001 (NEC & Hub) */
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
            hClassUsb, subName, 0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL,
            &hTargetClass, &disp
        );

        if (res == ERROR_SUCCESS) {
            StrCpy(chosenClass, subName);
            StrCpy(chosenClassPath, "USB\\");
            StrCat(chosenClassPath, subName);
            break;
        }
    }

    if (!hTargetClass) {
        RegCloseKey(hClassUsb);
        MessageBoxA(NULL, "Failed to create Services\\Class\\USB target key!", "Auto-Binder Error", MB_ICONERROR);
        ExitProcess(2);
    }

    /* Populate the Class\USB key */
    const char *devLoader = "*ntkern";
    const char *driverName = "RTWPROBE.SYS";
    const char *driverDesc = "Realtek 8822BU USB Wi-Fi Probe Driver";
    const char *matchId = "USB\\VID_0BDA&PID_B812";

    RegSetValueExA(hTargetClass, "DevLoader", 0, REG_SZ, (const BYTE *)devLoader, lstrlenA(devLoader) + 1);
    RegSetValueExA(hTargetClass, "NTMPDriver", 0, REG_SZ, (const BYTE *)driverName, lstrlenA(driverName) + 1);
    RegSetValueExA(hTargetClass, "DriverDesc", 0, REG_SZ, (const BYTE *)driverDesc, lstrlenA(driverDesc) + 1);
    RegSetValueExA(hTargetClass, "MatchingDeviceId", 0, REG_SZ, (const BYTE *)matchId, lstrlenA(matchId) + 1);
    RegCloseKey(hTargetClass);
    RegCloseKey(hClassUsb);

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
            const char *cls = "USB";
            const char *clsGuid = "{36fc9e60-c465-11cf-8056-444553540000}";
            DWORD cfgFlags = 0;

            RegSetValueExA(hInstKey, "Class", 0, REG_SZ, (const BYTE *)cls, lstrlenA(cls) + 1);
            RegSetValueExA(hInstKey, "ClassGUID", 0, REG_SZ, (const BYTE *)clsGuid, lstrlenA(clsGuid) + 1);
            RegSetValueExA(hInstKey, "DeviceDesc", 0, REG_SZ, (const BYTE *)driverDesc, lstrlenA(driverDesc) + 1);
            RegSetValueExA(hInstKey, "Driver", 0, REG_SZ, (const BYTE *)chosenClassPath, lstrlenA(chosenClassPath) + 1);
            RegSetValueExA(hInstKey, "DevLoader", 0, REG_SZ, (const BYTE *)devLoader, lstrlenA(devLoader) + 1);
            RegSetValueExA(hInstKey, "NTMPDriver", 0, REG_SZ, (const BYTE *)driverName, lstrlenA(driverName) + 1);
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

    StrCat(msg, "\nSUCCESS! Registry is now bound to RTWPROBE.SYS!\n\n"
                "Next Step:\n"
                "1. Go to Device Manager and click 'Refresh' (or unplug/replug the Wi-Fi dongle).\n"
                "2. Run RTWTEST.EXE!");

    MessageBoxA(NULL, msg, "Auto-Binder - Success", MB_ICONINFORMATION);
    ExitProcess(0);
}

