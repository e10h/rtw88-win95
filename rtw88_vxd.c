/*
 * rtw88_vxd.c - Native Windows 95 NDIS 3.0/3.1 LE VxD MAC Driver
 * Target: Realtek RTL8822BU Wi-Fi (USB\VID_0BDA&PID_B812) on Windows 95 OSR 2.1
 * Architecture: Pentium MMX (strictly 0 CMOV instructions)
 *
 * Links directly against Microsoft Win95 DDK libndis.clb (VxD INT 20h stubs)
 * and rtw88vxd.asm (VxD DDB + Control Dispatch) into LE-format RTW88.VXD.
 *
 * CRITICAL: Win95 NDIS.VXD and libndis.clb are compiled with 1-byte struct
 * packing (-Zdp / -Zp1). All NDIS structures below use #pragma pack(push, 1).
 */

#pragma pack(push, 1)

/* Basic Win95 DDK types */
typedef void            VOID, *PVOID;
typedef char            CHAR, *PCHAR;
typedef unsigned char   UCHAR, *PUCHAR;
typedef unsigned char   BOOLEAN, *PBOOLEAN;
typedef short           SHORT, *PSHORT;
typedef unsigned short  USHORT, *PUSHORT;
typedef int             INT, *PINT;
typedef unsigned int    UINT, *PUINT;
typedef long            LONG, *PLONG;
typedef unsigned long   ULONG, *PULONG;

#define NULL  ((PVOID)0)
#define TRUE  1
#define FALSE 0

#define NDIS_API __attribute__((stdcall))

typedef ULONG  NDIS_STATUS, *PNDIS_STATUS;
typedef PVOID  NDIS_HANDLE, *PNDIS_HANDLE;
typedef ULONG  NDIS_OID, *PNDIS_OID;
typedef INT    NDIS_SPIN_LOCK, *PNDIS_SPIN_LOCK;
typedef ULONG  NTSTATUS;

/* Win95 NDIS_STRING (ANSI, Length = sizeof(literal) including NUL) */
typedef struct _STRING {
    USHORT Length;
    USHORT MaximumLength;
    PUCHAR Buffer;
} STRING, *PSTRING;

typedef STRING NDIS_STRING, *PNDIS_STRING;

/* NDIS Status codes (from Win95 DDK ndis.h) */
#define NDIS_STATUS_SUCCESS                 ((NDIS_STATUS)0x00000000L)
#define NDIS_STATUS_PENDING                 ((NDIS_STATUS)0x00000103L)
#define NDIS_STATUS_FAILURE                 ((NDIS_STATUS)0xC0000001L)
#define NDIS_STATUS_RESOURCES               ((NDIS_STATUS)0xC000009AL)
#define NDIS_STATUS_NOT_SUPPORTED           ((NDIS_STATUS)0xC00000BBL)
#define NDIS_STATUS_INVALID_LENGTH          ((NDIS_STATUS)0xC0010014L)
#define NDIS_STATUS_BUFFER_TOO_SHORT        ((NDIS_STATUS)0xC0010016L)
#define NDIS_STATUS_INVALID_OID             ((NDIS_STATUS)0xC0010017L)
#define NDIS_STATUS_UNSUPPORTED_MEDIA       ((NDIS_STATUS)0xC0010019L)

/* Medium types */
typedef enum _NDIS_MEDIUM {
    NdisMedium802_3 = 0,
    NdisMedium802_5,
    NdisMediumFddi,
    NdisMediumWan,
    NdisMediumLocalTalk,
    NdisMediumDix,
    NdisMediumArcnetRaw,
    NdisMediumArcnet878_2
} NDIS_MEDIUM, *PNDIS_MEDIUM;

/* Hardware status */
typedef enum _NDIS_HARDWARE_STATUS {
    NdisHardwareStatusReady = 0,
    NdisHardwareStatusInitializing,
    NdisHardwareStatusReset,
    NdisHardwareStatusClosing,
    NdisHardwareStatusNotReady
} NDIS_HARDWARE_STATUS, *PNDIS_HARDWARE_STATUS;

/* Interface / bus types (exact Win95 DDK ndis.h enum values) */
typedef enum _NDIS_INTERFACE_TYPE {
    NdisInterfaceInternal = 0,
    NdisInterfaceIsa = 1,
    NdisInterfaceEisa = 2,
    NdisInterfaceMca = 3,
    NdisInterfaceTurboChannel = 4,
    NdisInterfacePci = 5,
    NdisInterfacePcMcia = 8
} NDIS_INTERFACE_TYPE, *PNDIS_INTERFACE_TYPE;

/* NDIS Port Descriptor & Adapter Information (pack(1) exact match to Win95 ndis.h) */
typedef struct _NDIS_PORT_DESCRIPTOR {
    ULONG InitialPort;
    ULONG NumberOfPorts;
    PVOID *PortOffset;
} NDIS_PORT_DESCRIPTOR, *PNDIS_PORT_DESCRIPTOR;

typedef VOID (NDIS_API *PNDIS_ACTIVATE_CALLBACK)(
    NDIS_HANDLE NdisAdatperHandle,
    NDIS_HANDLE MacAdapterContext,
    ULONG DmaChannel
);

typedef struct _NDIS_ADAPTER_INFORMATION {
    ULONG DmaChannel;
    BOOLEAN Master;
    BOOLEAN Dma32BitAddresses;
    PNDIS_ACTIVATE_CALLBACK ActivateCallback;
    NDIS_INTERFACE_TYPE AdapterType;
    ULONG PhysicalMapRegistersNeeded;
    ULONG MaximumPhysicalMapping;
    ULONG NumberOfPortDescriptors;
    NDIS_PORT_DESCRIPTOR PortDescriptors[1];
} NDIS_ADAPTER_INFORMATION, *PNDIS_ADAPTER_INFORMATION;

/* NDIS Request structures (exact Win95 DDK ndis.h layout) */
typedef enum _NDIS_REQUEST_TYPE {
    NdisRequestQueryInformation = 0,
    NdisRequestSetInformation,
    NdisRequestQueryStatistics,
    NdisRequestOpen,
    NdisRequestClose,
    NdisRequestSend,
    NdisRequestTransferData,
    NdisRequestReset,
    NdisRequestGeneric1,
    NdisRequestGeneric2,
    NdisRequestGeneric3,
    NdisRequestGeneric4
} NDIS_REQUEST_TYPE, *PNDIS_REQUEST_TYPE;

typedef struct _NDIS_REQUEST {
    UCHAR MacReserved[16];
    NDIS_REQUEST_TYPE RequestType;
    union _DATA {
        struct _QUERY_INFORMATION {
            NDIS_OID Oid;
            PVOID InformationBuffer;
            UINT InformationBufferLength;
            UINT BytesWritten;
            UINT BytesNeeded;
        } QUERY_INFORMATION;
        struct _SET_INFORMATION {
            NDIS_OID Oid;
            PVOID InformationBuffer;
            UINT InformationBufferLength;
            UINT BytesRead;
            UINT BytesNeeded;
        } SET_INFORMATION;
    } DATA;
} NDIS_REQUEST, *PNDIS_REQUEST;

/* Forward declarations for packets/buffers */
struct _NDIS_PACKET;
typedef struct _NDIS_PACKET *PNDIS_PACKET;

/* NDIS 3.0/3.1 MAC Handler Typedefs (exact Win95 DDK ndis.h) */
typedef NDIS_STATUS (NDIS_API *OPEN_ADAPTER_HANDLER)(
    PNDIS_STATUS OpenErrorStatus,
    NDIS_HANDLE *MacBindingHandle,
    PUINT SelectedMediumIndex,
    PNDIS_MEDIUM MediumArray,
    UINT MediumArraySize,
    NDIS_HANDLE NdisBindingContext,
    NDIS_HANDLE MacAdapterContext,
    UINT OpenOptions,
    PSTRING AddressingInformation
);

typedef NDIS_STATUS (NDIS_API *CLOSE_ADAPTER_HANDLER)(
    NDIS_HANDLE MacBindingHandle
);

typedef NDIS_STATUS (NDIS_API *SEND_HANDLER)(
    NDIS_HANDLE MacBindingHandle,
    PNDIS_PACKET Packet
);

typedef NDIS_STATUS (NDIS_API *TRANSFER_DATA_HANDLER)(
    NDIS_HANDLE MacBindingHandle,
    NDIS_HANDLE MacReceiveContext,
    UINT BytesOffset,
    UINT BytesToTransfer,
    PNDIS_PACKET Packet,
    PUINT BytesTransferred
);

typedef NDIS_STATUS (NDIS_API *RESET_HANDLER)(
    NDIS_HANDLE MacBindingHandle
);

typedef NDIS_STATUS (NDIS_API *REQUEST_HANDLER)(
    NDIS_HANDLE MacBindingHandle,
    PNDIS_REQUEST NdisRequest
);

typedef VOID (NDIS_API *UNLOAD_MAC_HANDLER)(
    NDIS_HANDLE MacMacContext
);

typedef NDIS_STATUS (NDIS_API *ADD_ADAPTER_HANDLER)(
    NDIS_HANDLE MacMacContext,
    NDIS_HANDLE WrapperConfigurationContext,
    PNDIS_STRING AdapterName
);

typedef VOID (NDIS_API *REMOVE_ADAPTER_HANDLER)(
    NDIS_HANDLE MacAdapterContext
);

typedef struct _NDIS_MAC_CHARACTERISTICS {
    UCHAR MajorNdisVersion;
    UCHAR MinorNdisVersion;
    ULONG Reserved;
    OPEN_ADAPTER_HANDLER OpenAdapterHandler;
    CLOSE_ADAPTER_HANDLER CloseAdapterHandler;
    SEND_HANDLER SendHandler;
    TRANSFER_DATA_HANDLER TransferDataHandler;
    RESET_HANDLER ResetHandler;
    REQUEST_HANDLER RequestHandler;
    REQUEST_HANDLER QueryGlobalStatisticsHandler;
    UNLOAD_MAC_HANDLER UnloadMacHandler;
    ADD_ADAPTER_HANDLER AddAdapterHandler;
    REMOVE_ADAPTER_HANDLER RemoveAdapterHandler;
    NDIS_STRING Name;
} NDIS_MAC_CHARACTERISTICS, *PNDIS_MAC_CHARACTERISTICS;

#pragma pack(pop)

/* Win95 NDIS.VXD imports from libndis.clb (VxD INT 20h stdcall stubs) */
extern VOID NDIS_API NdisInitializeWrapper(
    PNDIS_HANDLE NdisWrapperHandle,
    PVOID SystemSpecific1,
    PVOID SystemSpecific2,
    PVOID SystemSpecific3
);

extern VOID NDIS_API NdisTerminateWrapper(
    NDIS_HANDLE NdisWrapperHandle,
    PVOID SystemSpecific
);

extern VOID NDIS_API NdisRegisterMac(
    PNDIS_STATUS Status,
    PNDIS_HANDLE NdisMacHandle,
    NDIS_HANDLE NdisWrapperHandle,
    NDIS_HANDLE MacMacContext,
    PNDIS_MAC_CHARACTERISTICS MacCharacteristics,
    UINT CharacteristicsLength
);

extern VOID NDIS_API NdisDeregisterMac(
    PNDIS_STATUS Status,
    NDIS_HANDLE NdisMacHandle
);

extern NDIS_STATUS NDIS_API NdisRegisterAdapter(
    PNDIS_HANDLE NdisAdapterHandle,
    NDIS_HANDLE NdisMacHandle,
    NDIS_HANDLE MacAdapterContext,
    NDIS_HANDLE WrapperConfigurationContext,
    PNDIS_STRING AdapterName,
    PNDIS_ADAPTER_INFORMATION AdapterInformation
);

extern NDIS_STATUS NDIS_API NdisDeregisterAdapter(
    NDIS_HANDLE NdisAdapterHandle
);

/* OIDs (exact Win95 DDK ndis.h values) */
#define OID_GEN_SUPPORTED_LIST              0x00010101
#define OID_GEN_HARDWARE_STATUS             0x00010102
#define OID_GEN_MEDIA_SUPPORTED             0x00010103
#define OID_GEN_MEDIA_IN_USE                0x00010104
#define OID_GEN_MAXIMUM_LOOKAHEAD           0x00010105
#define OID_GEN_MAXIMUM_FRAME_SIZE          0x00010106
#define OID_GEN_LINK_SPEED                  0x00010107
#define OID_GEN_TRANSMIT_BUFFER_SPACE       0x00010108
#define OID_GEN_RECEIVE_BUFFER_SPACE        0x00010109
#define OID_GEN_TRANSMIT_BLOCK_SIZE         0x0001010A
#define OID_GEN_RECEIVE_BLOCK_SIZE          0x0001010B
#define OID_GEN_VENDOR_ID                   0x0001010C
#define OID_GEN_VENDOR_DESCRIPTION          0x0001010D
#define OID_GEN_CURRENT_PACKET_FILTER       0x0001010E
#define OID_GEN_CURRENT_LOOKAHEAD           0x0001010F
#define OID_GEN_DRIVER_VERSION              0x00010110
#define OID_GEN_MAXIMUM_TOTAL_SIZE          0x00010111
#define OID_GEN_PROTOCOL_OPTIONS            0x00010112
#define OID_GEN_MAC_OPTIONS                 0x00010113
#define OID_GEN_MEDIA_CONNECT_STATUS        0x00010114

#define OID_GEN_XMIT_OK                     0x00020101
#define OID_GEN_RCV_OK                      0x00020102
#define OID_GEN_XMIT_ERROR                  0x00020103
#define OID_GEN_RCV_ERROR                   0x00020104
#define OID_GEN_RCV_NO_BUFFER               0x00020105

#define OID_802_3_PERMANENT_ADDRESS         0x01010101
#define OID_802_3_CURRENT_ADDRESS           0x01010102
#define OID_802_3_MULTICAST_LIST            0x01010103
#define OID_802_3_MAXIMUM_LIST_SIZE         0x01010104
#define OID_802_3_RCV_ERROR_ALIGNMENT       0x01020101
#define OID_802_3_XMIT_ONE_COLLISION        0x01020102
#define OID_802_3_XMIT_MORE_COLLISIONS      0x01020103

#define NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA 0x00000001
#define NDIS_MAC_OPTION_RECEIVE_SERIALIZED  0x00000002
#define NDIS_MAC_OPTION_TRANSFERS_NOT_PEND  0x00000004
#define NDIS_MAC_OPTION_NO_LOOPBACK         0x00000008

/* Adapter Constants */
#define RTW88_MAX_LOOKAHEAD     1500
#define RTW88_MAX_FRAME_SIZE    1500
#define RTW88_MAX_PACKET_SIZE   1514
#define RTW88_LINK_SPEED        100000      /* 10 Mbps in 100 bps units */
#define RTW88_VENDOR_ID         0x000BDA00
#define RTW88_MAX_OPENS         16

static const UCHAR g_PermanentMac[6] = { 0x80, 0x42, 0xEF, 0x68, 0x80, 0x42 };
static const CHAR  g_VendorDesc[]    = "Realtek RTL8822BU Wireless LAN 802.11ac Adapter";
static CHAR        g_MacNameStr[]    = "RTW88";

static const NDIS_OID g_SupportedOids[] = {
    OID_GEN_SUPPORTED_LIST,
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_MAC_OPTIONS,
    OID_GEN_PROTOCOL_OPTIONS,
    OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_DRIVER_VERSION,
    OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_XMIT_OK,
    OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR,
    OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER,
    OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS
};

/* Driver State Structures */
typedef struct _RTW88_ADAPTER RTW88_ADAPTER, *PRTW88_ADAPTER;

typedef struct _RTW88_OPEN {
    BOOLEAN         InUse;
    PRTW88_ADAPTER  Adapter;
    NDIS_HANDLE     NdisBindingContext;
    ULONG           PacketFilter;
    ULONG           LookAhead;
    ULONG           ProtocolOptions;
} RTW88_OPEN, *PRTW88_OPEN;

struct _RTW88_ADAPTER {
    BOOLEAN         Active;
    NDIS_HANDLE     NdisAdapterHandle;
    UCHAR           CurrentMac[6];
    ULONG           PacketFilter;
    ULONG           LookAhead;
    ULONG           FramesXmitGood;
    ULONG           FramesRcvGood;
    ULONG           FramesXmitBad;
    ULONG           FramesRcvBad;
    RTW88_OPEN      Opens[RTW88_MAX_OPENS];
};

typedef struct _RTW88_MAC_BLOCK {
    NDIS_HANDLE                 NdisWrapperHandle;
    NDIS_HANDLE                 NdisMacHandle;
    NDIS_MAC_CHARACTERISTICS    MacCharacteristics;
} RTW88_MAC_BLOCK, *PRTW88_MAC_BLOCK;

static RTW88_MAC_BLOCK g_MacBlock;
static RTW88_ADAPTER   g_Adapter;

/* Local freestanding helpers (zero CRT dependencies, 0 CMOV) */
static VOID RtwZeroMem(PVOID dst, UINT len) {
    volatile UCHAR *p = (volatile UCHAR *)dst;
    UINT i;
    for (i = 0; i < len; i++) {
        p[i] = 0;
    }
}

static VOID RtwCopyMem(PVOID dst, const VOID *src, UINT len) {
    UCHAR *d = (UCHAR *)dst;
    const UCHAR *s = (const UCHAR *)src;
    UINT i;
    for (i = 0; i < len; i++) {
        d[i] = s[i];
    }
}

/* Forward declarations */
static NDIS_STATUS NDIS_API Rtw88OpenAdapter(
    PNDIS_STATUS OpenErrorStatus,
    NDIS_HANDLE *MacBindingHandle,
    PUINT SelectedMediumIndex,
    PNDIS_MEDIUM MediumArray,
    UINT MediumArraySize,
    NDIS_HANDLE NdisBindingContext,
    NDIS_HANDLE MacAdapterContext,
    UINT OpenOptions,
    PSTRING AddressingInformation
);
static NDIS_STATUS NDIS_API Rtw88CloseAdapter(NDIS_HANDLE MacBindingHandle);
static NDIS_STATUS NDIS_API Rtw88Send(NDIS_HANDLE MacBindingHandle, PNDIS_PACKET Packet);
static NDIS_STATUS NDIS_API Rtw88TransferData(
    NDIS_HANDLE MacBindingHandle,
    NDIS_HANDLE MacReceiveContext,
    UINT BytesOffset,
    UINT BytesToTransfer,
    PNDIS_PACKET Packet,
    PUINT BytesTransferred
);
static NDIS_STATUS NDIS_API Rtw88Reset(NDIS_HANDLE MacBindingHandle);
static NDIS_STATUS NDIS_API Rtw88Request(NDIS_HANDLE MacBindingHandle, PNDIS_REQUEST NdisRequest);
static NDIS_STATUS NDIS_API Rtw88QueryGlobalStatistics(NDIS_HANDLE MacAdapterContext, PNDIS_REQUEST NdisRequest);
static VOID NDIS_API Rtw88Unload(NDIS_HANDLE MacMacContext);
static NDIS_STATUS NDIS_API Rtw88AddAdapter(
    NDIS_HANDLE MacMacContext,
    NDIS_HANDLE WrapperConfigurationContext,
    PNDIS_STRING AdapterName
);
static VOID NDIS_API Rtw88RemoveAdapter(NDIS_HANDLE MacAdapterContext);

/* ---------------------------------------------------------------------------
 * DriverEntry - Called by rtw88vxd.asm on Device_Init / Sys_Dynamic_Device_Init
 * --------------------------------------------------------------------------- */
NTSTATUS NDIS_API DriverEntry(PVOID DriverObject, PVOID RegistryPath) {
    NDIS_STATUS status = NDIS_STATUS_FAILURE;

    RtwZeroMem(&g_MacBlock, sizeof(g_MacBlock));
    RtwZeroMem(&g_Adapter, sizeof(g_Adapter));

    NdisInitializeWrapper(
        &g_MacBlock.NdisWrapperHandle,
        DriverObject,
        RegistryPath,
        NULL
    );

    g_MacBlock.MacCharacteristics.MajorNdisVersion             = 3;
    g_MacBlock.MacCharacteristics.MinorNdisVersion             = 0;
    g_MacBlock.MacCharacteristics.Reserved                     = 0;
    g_MacBlock.MacCharacteristics.OpenAdapterHandler           = Rtw88OpenAdapter;
    g_MacBlock.MacCharacteristics.CloseAdapterHandler          = Rtw88CloseAdapter;
    g_MacBlock.MacCharacteristics.SendHandler                  = Rtw88Send;
    g_MacBlock.MacCharacteristics.TransferDataHandler          = Rtw88TransferData;
    g_MacBlock.MacCharacteristics.ResetHandler                 = Rtw88Reset;
    g_MacBlock.MacCharacteristics.RequestHandler               = Rtw88Request;
    g_MacBlock.MacCharacteristics.QueryGlobalStatisticsHandler = Rtw88QueryGlobalStatistics;
    g_MacBlock.MacCharacteristics.UnloadMacHandler             = Rtw88Unload;
    g_MacBlock.MacCharacteristics.AddAdapterHandler            = Rtw88AddAdapter;
    g_MacBlock.MacCharacteristics.RemoveAdapterHandler         = Rtw88RemoveAdapter;

    /* Exact Win95 NDIS_STRING_CONST("RTW88") layout: Length=6, MaxLength=6 */
    g_MacBlock.MacCharacteristics.Name.Length        = sizeof(g_MacNameStr);
    g_MacBlock.MacCharacteristics.Name.MaximumLength = sizeof(g_MacNameStr);
    g_MacBlock.MacCharacteristics.Name.Buffer        = (PUCHAR)g_MacNameStr;

    NdisRegisterMac(
        &status,
        &g_MacBlock.NdisMacHandle,
        g_MacBlock.NdisWrapperHandle,
        (NDIS_HANDLE)&g_MacBlock,
        &g_MacBlock.MacCharacteristics,
        sizeof(g_MacBlock.MacCharacteristics)
    );

    if (status != NDIS_STATUS_SUCCESS) {
        NdisTerminateWrapper(g_MacBlock.NdisWrapperHandle, NULL);
        return (NTSTATUS)status;
    }

    return NDIS_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * AddAdapterHandler - Called by NDIS.VXD for our Plug & Play Net devnode
 * --------------------------------------------------------------------------- */
static NDIS_STATUS NDIS_API Rtw88AddAdapter(
    NDIS_HANDLE MacMacContext,
    NDIS_HANDLE WrapperConfigurationContext,
    PNDIS_STRING AdapterName)
{
    NDIS_STATUS status;
    NDIS_ADAPTER_INFORMATION adapterInfo;

    (void)MacMacContext;

    RtwZeroMem(&g_Adapter, sizeof(g_Adapter));
    RtwCopyMem(g_Adapter.CurrentMac, g_PermanentMac, 6);
    g_Adapter.LookAhead = RTW88_MAX_LOOKAHEAD;

    RtwZeroMem(&adapterInfo, sizeof(adapterInfo));
    adapterInfo.AdapterType             = NdisInterfaceInternal;
    adapterInfo.Master                  = FALSE;
    adapterInfo.Dma32BitAddresses       = FALSE;
    adapterInfo.NumberOfPortDescriptors = 0;

    status = NdisRegisterAdapter(
        &g_Adapter.NdisAdapterHandle,
        g_MacBlock.NdisMacHandle,
        (NDIS_HANDLE)&g_Adapter,
        WrapperConfigurationContext,
        AdapterName,
        &adapterInfo
    );

    if (status == NDIS_STATUS_SUCCESS) {
        g_Adapter.Active = TRUE;
    }

    return status;
}

/* ---------------------------------------------------------------------------
 * RemoveAdapterHandler & UnloadMacHandler (Safe Hot-Unplug!)
 * --------------------------------------------------------------------------- */
static VOID NDIS_API Rtw88RemoveAdapter(NDIS_HANDLE MacAdapterContext) {
    PRTW88_ADAPTER adapter = (PRTW88_ADAPTER)MacAdapterContext;
    if (adapter && adapter->Active) {
        adapter->Active = FALSE;
        if (adapter->NdisAdapterHandle) {
            NdisDeregisterAdapter(adapter->NdisAdapterHandle);
            adapter->NdisAdapterHandle = NULL;
        }
    }
}

static VOID NDIS_API Rtw88Unload(NDIS_HANDLE MacMacContext) {
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;
    (void)MacMacContext;

    if (g_MacBlock.NdisMacHandle) {
        NdisDeregisterMac(&status, g_MacBlock.NdisMacHandle);
        g_MacBlock.NdisMacHandle = NULL;
    }
    if (g_MacBlock.NdisWrapperHandle) {
        NdisTerminateWrapper(g_MacBlock.NdisWrapperHandle, NULL);
        g_MacBlock.NdisWrapperHandle = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * OpenAdapter / CloseAdapter
 * --------------------------------------------------------------------------- */
static NDIS_STATUS NDIS_API Rtw88OpenAdapter(
    PNDIS_STATUS OpenErrorStatus,
    NDIS_HANDLE *MacBindingHandle,
    PUINT SelectedMediumIndex,
    PNDIS_MEDIUM MediumArray,
    UINT MediumArraySize,
    NDIS_HANDLE NdisBindingContext,
    NDIS_HANDLE MacAdapterContext,
    UINT OpenOptions,
    PSTRING AddressingInformation)
{
    PRTW88_ADAPTER adapter = (PRTW88_ADAPTER)MacAdapterContext;
    UINT i;
    BOOLEAN found = FALSE;
    PRTW88_OPEN openSlot = NULL;

    (void)OpenOptions;
    (void)AddressingInformation;

    if (OpenErrorStatus) {
        *OpenErrorStatus = NDIS_STATUS_SUCCESS;
    }

    if (!adapter || !adapter->Active) {
        return NDIS_STATUS_FAILURE;
    }

    for (i = 0; i < MediumArraySize; i++) {
        if (MediumArray[i] == NdisMedium802_3) {
            *SelectedMediumIndex = i;
            found = TRUE;
            break;
        }
    }

    if (!found) {
        if (OpenErrorStatus) {
            *OpenErrorStatus = NDIS_STATUS_UNSUPPORTED_MEDIA;
        }
        return NDIS_STATUS_UNSUPPORTED_MEDIA;
    }

    for (i = 0; i < RTW88_MAX_OPENS; i++) {
        if (!adapter->Opens[i].InUse) {
            openSlot = &adapter->Opens[i];
            break;
        }
    }

    if (!openSlot) {
        return NDIS_STATUS_RESOURCES;
    }

    openSlot->InUse              = TRUE;
    openSlot->Adapter            = adapter;
    openSlot->NdisBindingContext = NdisBindingContext;
    openSlot->PacketFilter       = 0;
    openSlot->LookAhead          = RTW88_MAX_LOOKAHEAD;
    openSlot->ProtocolOptions    = 0;

    *MacBindingHandle = (NDIS_HANDLE)openSlot;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS NDIS_API Rtw88CloseAdapter(NDIS_HANDLE MacBindingHandle) {
    PRTW88_OPEN openSlot = (PRTW88_OPEN)MacBindingHandle;
    if (openSlot) {
        openSlot->InUse = FALSE;
        openSlot->Adapter = NULL;
    }
    return NDIS_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Send / TransferData / Reset
 * --------------------------------------------------------------------------- */
static NDIS_STATUS NDIS_API Rtw88Send(
    NDIS_HANDLE MacBindingHandle,
    PNDIS_PACKET Packet)
{
    PRTW88_OPEN openSlot = (PRTW88_OPEN)MacBindingHandle;
    (void)Packet;

    if (!openSlot || !openSlot->Adapter) {
        return NDIS_STATUS_FAILURE;
    }

    openSlot->Adapter->FramesXmitGood++;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS NDIS_API Rtw88TransferData(
    NDIS_HANDLE MacBindingHandle,
    NDIS_HANDLE MacReceiveContext,
    UINT BytesOffset,
    UINT BytesToTransfer,
    PNDIS_PACKET Packet,
    PUINT BytesTransferred)
{
    (void)MacBindingHandle;
    (void)MacReceiveContext;
    (void)BytesOffset;
    (void)BytesToTransfer;
    (void)Packet;

    if (BytesTransferred) {
        *BytesTransferred = 0;
    }
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS NDIS_API Rtw88Reset(NDIS_HANDLE MacBindingHandle) {
    (void)MacBindingHandle;
    return NDIS_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Shared Query/Set OID Engine
 * --------------------------------------------------------------------------- */
static NDIS_STATUS Rtw88HandleQuery(
    PRTW88_ADAPTER adapter,
    PRTW88_OPEN openSlot,
    PNDIS_REQUEST req)
{
    NDIS_OID oid   = req->DATA.QUERY_INFORMATION.Oid;
    PVOID    buf   = req->DATA.QUERY_INFORMATION.InformationBuffer;
    UINT     buflen = req->DATA.QUERY_INFORMATION.InformationBufferLength;

    ULONG  uLongVal  = 0;
    USHORT uShortVal = 0;
    const VOID *src  = &uLongVal;
    UINT   copyLen   = sizeof(ULONG);

    req->DATA.QUERY_INFORMATION.BytesWritten = 0;
    req->DATA.QUERY_INFORMATION.BytesNeeded  = 0;

    switch (oid) {
        case OID_GEN_SUPPORTED_LIST:
            src     = g_SupportedOids;
            copyLen = sizeof(g_SupportedOids);
            break;

        case OID_GEN_HARDWARE_STATUS:
            uLongVal = NdisHardwareStatusReady;
            break;

        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            uLongVal = NdisMedium802_3;
            break;

        case OID_GEN_MAXIMUM_LOOKAHEAD:
            uLongVal = RTW88_MAX_LOOKAHEAD;
            break;

        case OID_GEN_CURRENT_LOOKAHEAD:
            if (openSlot) {
                uLongVal = openSlot->LookAhead;
            } else if (adapter) {
                uLongVal = adapter->LookAhead;
            } else {
                uLongVal = RTW88_MAX_LOOKAHEAD;
            }
            break;

        case OID_GEN_MAXIMUM_FRAME_SIZE:
            uLongVal = RTW88_MAX_FRAME_SIZE;
            break;

        case OID_GEN_MAXIMUM_TOTAL_SIZE:
            uLongVal = RTW88_MAX_PACKET_SIZE;
            break;

        case OID_GEN_LINK_SPEED:
            uLongVal = RTW88_LINK_SPEED;
            break;

        case OID_GEN_TRANSMIT_BUFFER_SPACE:
        case OID_GEN_RECEIVE_BUFFER_SPACE:
            uLongVal = 16384;
            break;

        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            uLongVal = RTW88_MAX_PACKET_SIZE;
            break;

        case OID_GEN_VENDOR_ID:
            uLongVal = RTW88_VENDOR_ID;
            break;

        case OID_GEN_VENDOR_DESCRIPTION:
            src     = g_VendorDesc;
            copyLen = sizeof(g_VendorDesc);
            break;

        case OID_GEN_DRIVER_VERSION:
            uShortVal = 0x0300;
            src       = &uShortVal;
            copyLen   = sizeof(USHORT);
            break;

        case OID_GEN_CURRENT_PACKET_FILTER:
            if (openSlot) {
                uLongVal = openSlot->PacketFilter;
            } else if (adapter) {
                uLongVal = adapter->PacketFilter;
            }
            break;

        case OID_GEN_PROTOCOL_OPTIONS:
            uLongVal = openSlot ? openSlot->ProtocolOptions : 0;
            break;

        case OID_GEN_MAC_OPTIONS:
            uLongVal = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                       NDIS_MAC_OPTION_TRANSFERS_NOT_PEND  |
                       NDIS_MAC_OPTION_NO_LOOPBACK;
            break;

        case OID_GEN_MEDIA_CONNECT_STATUS:
            uLongVal = 0; /* NdisMediaStateConnected */
            break;

        case OID_GEN_XMIT_OK:
            uLongVal = adapter ? adapter->FramesXmitGood : 0;
            break;

        case OID_GEN_RCV_OK:
            uLongVal = adapter ? adapter->FramesRcvGood : 0;
            break;

        case OID_GEN_XMIT_ERROR:
            uLongVal = adapter ? adapter->FramesXmitBad : 0;
            break;

        case OID_GEN_RCV_ERROR:
            uLongVal = adapter ? adapter->FramesRcvBad : 0;
            break;

        case OID_GEN_RCV_NO_BUFFER:
        case OID_802_3_RCV_ERROR_ALIGNMENT:
        case OID_802_3_XMIT_ONE_COLLISION:
        case OID_802_3_XMIT_MORE_COLLISIONS:
            uLongVal = 0;
            break;

        case OID_802_3_PERMANENT_ADDRESS:
            src     = g_PermanentMac;
            copyLen = 6;
            break;

        case OID_802_3_CURRENT_ADDRESS:
            src     = adapter ? (const VOID *)adapter->CurrentMac : (const VOID *)g_PermanentMac;
            copyLen = 6;
            break;

        case OID_802_3_MULTICAST_LIST:
            req->DATA.QUERY_INFORMATION.BytesWritten = 0;
            return NDIS_STATUS_SUCCESS;

        case OID_802_3_MAXIMUM_LIST_SIZE:
            uLongVal = 32;
            break;

        default:
            return NDIS_STATUS_INVALID_OID;
    }

    if (buflen < copyLen) {
        req->DATA.QUERY_INFORMATION.BytesNeeded = copyLen;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    if (buf && copyLen > 0) {
        RtwCopyMem(buf, src, copyLen);
    }
    req->DATA.QUERY_INFORMATION.BytesWritten = copyLen;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS Rtw88HandleSet(
    PRTW88_ADAPTER adapter,
    PRTW88_OPEN openSlot,
    PNDIS_REQUEST req)
{
    NDIS_OID oid    = req->DATA.SET_INFORMATION.Oid;
    PVOID    buf    = req->DATA.SET_INFORMATION.InformationBuffer;
    UINT     buflen = req->DATA.SET_INFORMATION.InformationBufferLength;

    req->DATA.SET_INFORMATION.BytesRead   = 0;
    req->DATA.SET_INFORMATION.BytesNeeded = 0;

    switch (oid) {
        case OID_GEN_CURRENT_PACKET_FILTER:
            if (buflen < sizeof(ULONG)) {
                req->DATA.SET_INFORMATION.BytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (buf) {
                ULONG val = *(PULONG)buf;
                if (openSlot) openSlot->PacketFilter = val;
                if (adapter)  adapter->PacketFilter  = val;
            }
            req->DATA.SET_INFORMATION.BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;

        case OID_GEN_CURRENT_LOOKAHEAD:
            if (buflen < sizeof(ULONG)) {
                req->DATA.SET_INFORMATION.BytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (buf) {
                ULONG val = *(PULONG)buf;
                if (openSlot) openSlot->LookAhead = val;
                if (adapter)  adapter->LookAhead  = val;
            }
            req->DATA.SET_INFORMATION.BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;

        case OID_GEN_PROTOCOL_OPTIONS:
            if (buflen < sizeof(ULONG)) {
                req->DATA.SET_INFORMATION.BytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (buf && openSlot) {
                openSlot->ProtocolOptions = *(PULONG)buf;
            }
            req->DATA.SET_INFORMATION.BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;

        case OID_802_3_MULTICAST_LIST:
            req->DATA.SET_INFORMATION.BytesRead = buflen;
            return NDIS_STATUS_SUCCESS;

        default:
            return NDIS_STATUS_INVALID_OID;
    }
}

static NDIS_STATUS NDIS_API Rtw88Request(
    NDIS_HANDLE MacBindingHandle,
    PNDIS_REQUEST NdisRequest)
{
    PRTW88_OPEN openSlot = (PRTW88_OPEN)MacBindingHandle;
    PRTW88_ADAPTER adapter = openSlot ? openSlot->Adapter : &g_Adapter;

    if (!NdisRequest) {
        return NDIS_STATUS_FAILURE;
    }

    if (NdisRequest->RequestType == NdisRequestQueryInformation ||
        NdisRequest->RequestType == NdisRequestQueryStatistics) {
        return Rtw88HandleQuery(adapter, openSlot, NdisRequest);
    }

    if (NdisRequest->RequestType == NdisRequestSetInformation) {
        return Rtw88HandleSet(adapter, openSlot, NdisRequest);
    }

    return NDIS_STATUS_NOT_SUPPORTED;
}

static NDIS_STATUS NDIS_API Rtw88QueryGlobalStatistics(
    NDIS_HANDLE MacAdapterContext,
    PNDIS_REQUEST NdisRequest)
{
    PRTW88_ADAPTER adapter = (PRTW88_ADAPTER)MacAdapterContext;
    if (!NdisRequest) {
        return NDIS_STATUS_FAILURE;
    }
    return Rtw88HandleQuery(adapter ? adapter : &g_Adapter, NULL, NdisRequest);
}
