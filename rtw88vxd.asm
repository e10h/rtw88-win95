;=============================================================================
; rtw88vxd.asm - Windows 95 Native LE VxD Shell for RTW88 NDIS 3.x MAC Driver
; Matches Microsoft Win95 DDK ELNK3.VXD layout: DDB + code in 1 locked LCODE obj
;=============================================================================
    .386P

DDK_VERSION             EQU 030Ah       ; Exact match to Win95 DDK ndis.mk (-DWIN31COMPAT) & ELNK3.VXD
UNDEFINED_DEVICE_ID     EQU 0000h
MAC_INIT_ORDER          EQU 54004000h   ; VNETBIOS (54000000h) + NDIS (2000h) + MAC (2000h)

DEVICE_INIT             EQU 0001h
SYS_DYNAMIC_DEVICE_INIT EQU 001Bh
SYS_DYNAMIC_DEVICE_EXIT EQU 001Ch
W32_DEVICEIOCONTROL     EQU 0023h

NDIS_GETVERSION_SVC     EQU 00280000h   ; VxD 0028h (NDIS), Service 0000h (NdisGetVersion)

;-----------------------------------------------------------------------------
; Single Locked Code+Data Segment (_LTEXT, class CODE -> Obj 1 = 0x00002045)
; RTW88_DDB MUST be at offset 0 of _LTEXT so jwlink SetHeaderVxDInfo reads it.
;-----------------------------------------------------------------------------
_LTEXT SEGMENT DWORD PUBLIC USE32 'CODE'
    ASSUME CS:FLAT, DS:FLAT, ES:FLAT, SS:FLAT

PUBLIC RTW88_DDB
RTW88_DDB LABEL BYTE
    dd 0                        ; +00 DDB_Next
    dw DDK_VERSION              ; +04 DDB_SDK_Version (030Ah)
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

PUBLIC _g_VxdInitCalls
_g_VxdInitCalls     dd 0
PUBLIC _g_VxdNdisCarry
_g_VxdNdisCarry     dd 0
PUBLIC _g_VxdNdisVer
_g_VxdNdisVer       dd 0
PUBLIC bInitAlready
bInitAlready        db 0
                    db 0, 0, 0

EXTERN _DriverEntry@8:NEAR
EXTERN _Rtw88DeviceIoControl@4:NEAR

C_Device_Init PROC NEAR
    pushad
    inc  dword ptr [_g_VxdInitCalls]
    mov  al, bInitAlready
    cmp  al, 0
    jnz  Succeed_Init_Phase
    inc  bInitAlready

    ; Verify NDIS.VXD is loaded: VxDcall NdisGetVersion
    int  20h
    dd   NDIS_GETVERSION_SVC
    mov  [_g_VxdNdisVer], eax
    jc   Ndis_Not_Loaded
    mov  dword ptr [_g_VxdNdisCarry], 0

    ; Call C DriverEntry(0, 0) (__stdcall)
    push 0
    push 0
    call _DriverEntry@8
    jmp  Succeed_Init_Phase

Ndis_Not_Loaded:
    mov  dword ptr [_g_VxdNdisCarry], 1

Succeed_Init_Phase:
    popad
    clc
    ret
C_Device_Init ENDP

C_W32_Dioc PROC NEAR
    push ebx
    push esi
    push edi
    push ebp
    push esi                    ; arg1 = PDIOCPARAMETERS (in ESI)
    call _Rtw88DeviceIoControl@4
    pop  ebp
    pop  edi
    pop  esi
    pop  ebx
    clc
    ret
C_W32_Dioc ENDP

PUBLIC RTW88_Control
RTW88_Control PROC NEAR
    cmp  eax, DEVICE_INIT
    je   C_Device_Init
    cmp  eax, SYS_DYNAMIC_DEVICE_INIT
    je   C_Device_Init
    cmp  eax, W32_DEVICEIOCONTROL
    je   C_W32_Dioc
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
