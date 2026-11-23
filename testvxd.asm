; testvxd.asm - Minimal 1-object Win95 dynamic VxD (matches ELNK3.VXD layout 100%)
    .386P

; Both DDB and Control proc live in ONE locked CODE object (Obj 1, 0x00002045)
; matching Microsoft LINK.EXE -VXD / ELNK3.VXD / SWAPCOMP.VXD / AHCIFIX.386!
_LTEXT SEGMENT DWORD PUBLIC USE32 'CODE'
    ASSUME CS:FLAT, DS:FLAT, ES:FLAT, SS:FLAT

PUBLIC TESTVXD_DDB
TESTVXD_DDB LABEL BYTE
    dd 0                        ; DDB_Next
    dw 030Ah                    ; DDB_SDK_Version (3.10, exact match to ELNK3.VXD)
    dw 0000h                    ; DDB_Req_Device_Number
    db 1                        ; Major
    db 0                        ; Minor
    dw 0                        ; Flags
    db 'TESTVXD '               ; Name (8 chars)
    dd 80000000h                ; UNDEFINED_INIT_ORDER
    dd OFFSET TESTVXD_Control   ; DDB_Control_Proc
    dd 0, 0, 0, 0, 0, 0, 0, 0
    dd 'Prev'
    dd 50h
    dd 'Rsv1'
    dd 'Rsv2'
    dd 'Rsv3'

PUBLIC TESTVXD_Control
TESTVXD_Control PROC NEAR
    cmp  eax, 0023h             ; W32_DEVICEIOCONTROL
    je   short Do_Dioc
    clc
    ret
Do_Dioc:
    xor  eax, eax               ; return ERROR_SUCCESS (0)
    clc
    ret
TESTVXD_Control ENDP

_LTEXT ENDS
END
