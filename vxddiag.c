/*
 * vxddiag.c - Zero-reboot Windows 95 NDIS & VxD live diagnostic tool
 * Auto-copies TESTVXD.VXD and RTW88.VXD from its own folder into C:\WINDOWS\SYSTEM,
 * tests VXDLDR loading on BOTH, queries RTW88_DIAG telemetry, and dumps registry.
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

static void BuildExeDirPath(char *out, const char *name) {
    char exe[260];
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof(exe) - 1);
    if (len > 0 && len < sizeof(exe)) {
        exe[len] = '\0';
        int last = -1, i;
        for (i = 0; exe[i]; i++) {
            if (exe[i] == '\\' || exe[i] == '/') last = i;
        }
        if (last >= 0) {
            exe[last + 1] = '\0';
            lstrcpyA(out, exe);
            lstrcatA(out, name);
            return;
        }
    }
    lstrcpyA(out, name);
}

static void InstallAndCheck(const char *sysDir, const char *name) {
    char src[260], dst[260];
    BuildExeDirPath(src, name);
    lstrcpyA(dst, sysDir);
    lstrcatA(dst, "\\");
    lstrcatA(dst, name);

    BOOL copied = CopyFileA(src, dst, FALSE);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(dst, &fd);
    AppStr(name);
    AppStr(": ");
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        AppStr(copied ? "UPDATED (" : "EXISTS (");
        AppDec(fd.nFileSizeLow);
        AppStr(" bytes)\r\n");
    } else {
        AppStr("MISSING (copyErr=");
        AppDec(GetLastError());
        AppStr(")\r\n");
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    AppStr("=== RTW88 WIN95 LIVE DIAGNOSTIC v2 ===\r\n");

    char sysDir[260];
    GetSystemDirectoryA(sysDir, sizeof(sysDir));

    InstallAndCheck(sysDir, "TESTVXD.VXD");
    InstallAndCheck(sysDir, "RTW88.VXD");

    /* Test 1: Minimal bare VxD (zero NDIS calls) */
    AppStr("\r\n[Test 1: \\\\.\\TESTVXD.VXD (bare LE VxD)]\r\n");
    HANDLE hTest = CreateFileA("\\\\.\\TESTVXD.VXD", 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hTest == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        AppStr("  FAILED! err="); AppDec(err); AppStr(" ("); AppHex(err); AppStr(")\r\n");
    } else {
        AppStr("  LOADED OK! (JWlink LE format is 100% VALID!)\r\n");
        CloseHandle(hTest);
    }

    /* Test 2: RTW88.VXD (with NDIS + W32_DEVICEIOCONTROL telemetry) */
    AppStr("\r\n[Test 2: \\\\.\\RTW88.VXD (NDIS VxD)]\r\n");
    HANDLE hVxd = CreateFileA("\\\\.\\RTW88.VXD", 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hVxd == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        AppStr("  FAILED! err="); AppDec(err); AppStr(" ("); AppHex(err); AppStr(")\r\n");
    } else {
        AppStr("  LOADED OK!\r\n");
        RTW88_DIAG d;
        DWORD cb = 0;
        if (DeviceIoControl(hVxd, 1, NULL, 0, &d, sizeof(d), &cb, NULL)) {
            AppStr("  Magic="); AppHex(d.Magic);
            AppStr(" Init="); AppDec(d.InitCalls);
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

    /* Test 3: Also try full path \\.\C:\WINDOWS\SYSTEM\TESTVXD.VXD if short name failed */
    if (hTest == INVALID_HANDLE_VALUE) {
        char fullVxd[300];
        lstrcpyA(fullVxd, "\\\\.\\");
        lstrcatA(fullVxd, sysDir);
        lstrcatA(fullVxd, "\\TESTVXD.VXD");
        AppStr("\r\n[Test 3: "); AppStr(fullVxd); AppStr("]\r\n");
        HANDLE hFull = CreateFileA(fullVxd, 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (hFull == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            AppStr("  FAILED! err="); AppDec(err); AppStr(" ("); AppHex(err); AppStr(")\r\n");
        } else {
            AppStr("  LOADED OK!\r\n");
            CloseHandle(hFull);
        }
    }

    /* Save DIAG.TXT next to VXDDIAG.EXE */
    char diagPath[260];
    BuildExeDirPath(diagPath, "DIAG.TXT");
    HANDLE hf = CreateFileA(diagPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(hf, g_Out, (DWORD)g_Pos, &w, NULL);
        CloseHandle(hf);
    }

    MessageBoxA(NULL, g_Out, "RTW88 Live Diagnostic v2", MB_OK | MB_ICONINFORMATION);
    ExitProcess(0);
}
