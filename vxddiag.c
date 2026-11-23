/*
 * vxddiag.c - Zero-reboot Windows 95 NDIS & VxD live diagnostic tool
 * Directly tests VXDLDR loading of RTW88.VXD, queries NDIS.VXD status,
 * reads internal RTW88_DIAG telemetry via W32_DEVICEIOCONTROL, and dumps
 * the exact Enum + Services\Class\Net + Services\VxD\NDIS registry state.
 */
#include <windows.h>

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
} RTW88_DIAG;
#pragma pack(pop)

static char g_Out[8192];
static int  g_Pos = 0;

static void AppStr(const char *s) {
    while (*s && g_Pos < (int)sizeof(g_Out) - 2) {
        g_Out[g_Pos++] = *s++;
    }
    g_Out[g_Pos] = '\0';
}

static void AppHex(DWORD v) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    int i;
    for (i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> (28 - i * 4)) & 0xF];
    }
    buf[10] = '\0';
    AppStr(buf);
}

static void AppDec(DWORD v) {
    char buf[16];
    int n = 0;
    if (v == 0) { AppStr("0"); return; }
    while (v > 0 && n < 15) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        char c[2] = { buf[--n], 0 };
        AppStr(c);
    }
}

static void CheckFile(const char *label, const char *path) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);
    AppStr(label);
    AppStr(": ");
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        AppStr("OK (");
        AppDec(fd.nFileSizeLow);
        AppStr(" bytes)\r\n");
    } else {
        AppStr("MISSING (err=");
        AppDec(GetLastError());
        AppStr(")\r\n");
    }
}

static void DumpRegStr(HKEY hKey, const char *valName) {
    char buf[160];
    DWORD len = sizeof(buf) - 1;
    DWORD type = 0;
    if (RegQueryValueExA(hKey, valName, NULL, &type, (LPBYTE)buf, &len) == ERROR_SUCCESS) {
        AppStr("  ");
        AppStr(valName);
        AppStr("=");
        if (type == REG_SZ) {
            buf[len] = '\0';
            AppStr(buf);
        } else {
            AppStr("(bin len=");
            AppDec(len);
            if (len >= 4) { AppStr(" val="); AppHex(*(DWORD*)buf); }
            else if (len == 1) { AppStr(" val="); AppHex((BYTE)buf[0]); }
            AppStr(")");
        }
        AppStr("\r\n");
    }
}

static void DumpRegKey(HKEY root, const char *subPath, const char *title) {
    HKEY h;
    AppStr("["); AppStr(title); AppStr("]\r\n");
    if (RegOpenKeyExA(root, subPath, 0, KEY_READ, &h) != ERROR_SUCCESS) {
        AppStr("  (key not found)\r\n");
        return;
    }
    DumpRegStr(h, "Class");
    DumpRegStr(h, "Driver");
    DumpRegStr(h, "DevLoader");
    DumpRegStr(h, "DeviceVxDs");
    DumpRegStr(h, "NTMPDriver");
    DumpRegStr(h, "StaticVxD");
    DumpRegStr(h, "DriverDesc");
    DumpRegStr(h, "MatchingDeviceId");
    DumpRegStr(h, "ConfigFlags");
    DumpRegStr(h, "LogDriverName");
    RegCloseKey(h);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    AppStr("=== RTW88 WIN95 LIVE DIAGNOSTIC ===\r\n");

    char sysDir[260];
    char path[300];
    GetSystemDirectoryA(sysDir, sizeof(sysDir));

    path[0] = 0; lstrcpyA(path, sysDir); lstrcatA(path, "\\NDIS.VXD");
    CheckFile("NDIS.VXD", path);

    path[0] = 0; lstrcpyA(path, sysDir); lstrcatA(path, "\\RTW88.VXD");
    CheckFile("RTW88.VXD", path);

    path[0] = 0; lstrcpyA(path, sysDir); lstrcatA(path, "\\VNETSUP.VXD");
    CheckFile("VNETSUP.VXD", path);

    /* Check if Services\VxD\NDIS exists */
    DumpRegKey(HKEY_LOCAL_MACHINE, "System\\CurrentControlSet\\Services\\VxD\\NDIS", "Services\\VxD\\NDIS");

    /* Test loading RTW88.VXD directly via VXDLDR! */
    AppStr("[VXDLDR Test: CreateFile \\\\.\\RTW88.VXD]\r\n");
    HANDLE hVxd = CreateFileA("\\\\.\\RTW88.VXD", 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hVxd == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        AppStr("  FAILED! GetLastError=");
        AppDec(err);
        AppStr(" ("); AppHex(err); AppStr(")\r\n");
    } else {
        AppStr("  LOADED OK!\r\n");
        RTW88_DIAG d;
        DWORD cb = 0;
        if (DeviceIoControl(hVxd, 1, NULL, 0, &d, sizeof(d), &cb, NULL)) {
            AppStr("  Magic="); AppHex(d.Magic);
            AppStr(" InitCalls="); AppDec(d.InitCalls);
            AppStr(" NdisCarry="); AppDec(d.NdisGetVerCarry);
            AppStr(" NdisVer="); AppHex(d.NdisVersion); AppStr("\r\n");
            AppStr("  DrvEntry="); AppDec(d.DriverEntryCalls);
            AppStr(" Wrap="); AppHex(d.WrapperHandle);
            AppStr(" RegMac="); AppHex(d.RegMacStatus);
            AppStr(" MacH="); AppHex(d.MacHandle); AppStr("\r\n");
            AppStr("  AddAdp="); AppDec(d.AddAdapterCalls);
            AppStr(" RegAdp="); AppHex(d.RegAdapterStatus);
            AppStr(" AdpH="); AppHex(d.AdapterHandle); AppStr("\r\n");
        } else {
            AppStr("  DIOC failed err="); AppDec(GetLastError()); AppStr("\r\n");
        }
        CloseHandle(hVxd);
    }

    /* Dump Enum\USB\VID_0BDA&PID_B812 first instance */
    HKEY hVid;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Enum\\USB\\VID_0BDA&PID_B812", 0, KEY_READ, &hVid) == ERROR_SUCCESS) {
        char inst[128];
        DWORD ilen = sizeof(inst);
        if (RegEnumKeyExA(hVid, 0, inst, &ilen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            char full[256];
            lstrcpyA(full, "Enum\\USB\\VID_0BDA&PID_B812\\");
            lstrcatA(full, inst);
            DumpRegKey(HKEY_LOCAL_MACHINE, full, full);

            /* Also read Driver= and dump that Services\Class key */
            HKEY hI;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, full, 0, KEY_READ, &hI) == ERROR_SUCCESS) {
                char drv[64];
                DWORD dlen = sizeof(drv);
                if (RegQueryValueExA(hI, "Driver", NULL, NULL, (LPBYTE)drv, &dlen) == ERROR_SUCCESS) {
                    char cls[160];
                    lstrcpyA(cls, "System\\CurrentControlSet\\Services\\Class\\");
                    lstrcatA(cls, drv);
                    DumpRegKey(HKEY_LOCAL_MACHINE, cls, cls);
                    lstrcatA(cls, "\\NDIS");
                    DumpRegKey(HKEY_LOCAL_MACHINE, cls, "NDIS subkey");
                }
                RegCloseKey(hI);
            }
        }
        RegCloseKey(hVid);
    } else {
        AppStr("[Enum\\USB\\VID_0BDA&PID_B812] NOT FOUND\r\n");
    }

    /* Also dump Net\0000 and Net\0001 to compare against existing working adapters */
    DumpRegKey(HKEY_LOCAL_MACHINE, "System\\CurrentControlSet\\Services\\Class\\Net\\0000", "Class\\Net\\0000");

    /* Write DIAG.TXT in current dir */
    HANDLE hf = CreateFileA("DIAG.TXT", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(hf, g_Out, (DWORD)g_Pos, &w, NULL);
        CloseHandle(hf);
    }

    MessageBoxA(NULL, g_Out, "RTW88 Live Diagnostic (saved DIAG.TXT)", MB_OK | MB_ICONINFORMATION);
    ExitProcess(0);
}
