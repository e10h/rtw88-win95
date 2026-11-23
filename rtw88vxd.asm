;=============================================================================
; rtw88vxd.asm - Windows 95 Native LE VxD Shell for RTW88 NDIS 3.x MAC Driver
; Equivalent to Microsoft Win95 DDK ndis3\src\ndisdev.asm (MAC=RTW88, CHICAGO)
; Assembled with JWasm (-coff) and linked with JWlink (format windows vxd dynamic)
;=============================================================================
    .386P

; VMM / NDIS constants (from Win95 DDK vmm.inc, netvxd.inc, ndis.inc)
DDK_VERSION             EQU 0400h
UNDEFINED_DEVICE_ID     EQU 0000h
MAC_INIT_ORDER          EQU 54004000h   ; VNETBIOS (54000000h) + NDIS (2000h) + MAC (2000h)

DEVICE_INIT             EQU 0001h
SYS_DYNAMIC_DEVICE_INIT EQU 001Bh
SYS_DYNAMIC_DEVICE_EXIT EQU 001Ch

NDIS_GETVERSION_SVC     EQU 00280000h   ; VxD 0028h (NDIS), Service 0000h (NdisGetVersion)
NDIS_STATUS_SUCCESS     EQU 00000000h

;-----------------------------------------------------------------------------
; Locked Data Segment (_LDATA, class DATA -> READ|WRITE|PRELOAD|BIG/32BIT)
; RTW88_DDB MUST be at offset 0 of _LDATA so jwlink SetHeaderVxDInfo reads it.
;-----------------------------------------------------------------------------
_LDATA SEGMENT DWORD PUBLIC USE32 'DATA'

PUBLIC RTW88_DDB
RTW88_DDB LABEL BYTE
    dd 0                        ; +00 DDB_Next
    dw DDK_VERSION              ; +04 DDB_SDK_Version (0400h = Win95)
    dw UNDEFINED_DEVICE_ID      ; +06 DDB_Req_Device_Number (0)
    db 3                        ; +08 DDB_Dev_Major_Version
    db 10                       ; +09 DDB_Dev_Minor_Version
    dw 0                        ; +0A DDB_Flags
    db 'RTW88   '               ; +0C DDB_Name (8 bytes, space padded)
    dd MAC_INIT_ORDER           ; +14 DDB_Init_Order
    dd OFFSET RTW88_Control     ; +18 DDB_Control_Proc
    dd 0                        ; +1C DDB_V86_API_Proc
    dd 0                        ; +20 DDB_PM_API_Proc
    dd 0                        ; +24 DDB_V86_API_CSIP
    dd 0                        ; +28 DDB_PM_API_CSIP
    dd 0                        ; +2C DDB_Reference_Data
    dd 0                        ; +30 DDB_Service_Table_Ptr
    dd 0                        ; +34 DDB_Service_Table_Size
    dd 0                        ; +38 DDB_Win32_Service_Table
    dd 'Prev'                   ; +3C DDB_Prev
    dd 50h                      ; +40 DDB_Size (80 bytes)
    dd 'Rsv1'                   ; +44 DDB_Reserved1
    dd 'Rsv2'                   ; +48 DDB_Reserved2
    dd 'Rsv3'                   ; +4C DDB_Reserved3

PUBLIC bInitAlready
bInitAlready db 0
    db 0, 0, 0                  ; dword padding

_LDATA ENDS

;-----------------------------------------------------------------------------
; Locked Code Segment (_LTEXT, class CODE -> READ|EXEC|PRELOAD|BIG/32BIT)
;-----------------------------------------------------------------------------
_LTEXT SEGMENT DWORD PUBLIC USE32 'CODE'
    ASSUME CS:FLAT, DS:FLAT, ES:FLAT, SS:FLAT

EXTERN _DriverEntry@8:NEAR

C_Device_Init PROC NEAR
    pushad
    mov  al, bInitAlready
    cmp  al, 0
    jnz  Succeed_Init_Phase
    inc  bInitAlready

    ; Verify NDIS.VXD is loaded: VxDcall NdisGetVersion
    int  20h
    dd   NDIS_GETVERSION_SVC
    jc   Fail_Init_Phase

    ; Call C DriverEntry(0, 0) (__stdcall)
    push 0
    push 0
    call _DriverEntry@8

    cmp  eax, NDIS_STATUS_SUCCESS
    jne  Fail_Init_Phase

Succeed_Init_Phase:
    popad
    clc
    ret

Fail_Init_Phase:
    mov  bInitAlready, 0
    popad
    stc
    ret
C_Device_Init ENDP

PUBLIC RTW88_Control
RTW88_Control PROC NEAR
    cmp  eax, DEVICE_INIT
    je   C_Device_Init
    cmp  eax, SYS_DYNAMIC_DEVICE_INIT
    je   C_Device_Init
    cmp  eax, SYS_DYNAMIC_DEVICE_EXIT
    je   Control_Exit
    clc
    ret
Control_Exit:
    mov  bInitAlready, 0
    clc
    ret
RTW88_Control ENDP

_LTEXT ENDS

END
