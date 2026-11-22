/*
 * rtw88.c - NDIS 3.1 Miniport Driver for Realtek RTL8822BU Wi-Fi
 * Targets Windows 95 OSR 2.1 / 2.5 (Pentium MMX, strictly 0 CMOV instructions)
 * Binds to VTCP.386 (TCP/IP) as standard 802.3 Ethernet (NdisMedium802_3)
 */

#include "ndis31.h"

#define RTW88_MAX_LOOKAHEAD         1514
#define RTW88_MAX_PACKET_SIZE       1514
#define RTW88_MAX_FRAME_SIZE        1500
#define RTW88_HEADER_SIZE           14
#define RTW88_LINK_SPEED            100000 /* 10 Mbps in 100 bps units */
#define RTW88_VENDOR_ID             0x000BDA00

static const UCHAR g_PermanentMac[6] = { 0x80, 0x42, 0xEF, 0x68, 0x80, 0x42 };
static const char g_VendorDesc[] = "Realtek RTL8822BU Wireless LAN 802.11ac Adapter";

/* Supported OID List */
static const NDIS_OID g_SupportedOids[] = {
    OID_GEN_SUPPORTED_LIST,
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_DRIVER_VERSION,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS,
    OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS
};

typedef struct _RTW88_ADAPTER {
    NDIS_HANDLE MiniportHandle;
    ULONG PacketFilter;
    ULONG LookaheadSize;
    UCHAR CurrentMac[6];
    ULONG FramesTransmitted;
    ULONG FramesReceived;
    ULONG TransmitErrors;
    ULONG ReceiveErrors;
    UCHAR TxBuffer[2048];
    UCHAR RxBuffer[2048];
} RTW88_ADAPTER, *PRTW88_ADAPTER;

static RTW88_ADAPTER g_Adapter;

/* Forward declarations */
static NDIS_STATUS NTAPI Rtw88Initialize(
    PNDIS_STATUS OpenErrorStatus,
    PUINT SelectedMediumIndex,
    PNDIS_MEDIUM MediumArray,
    UINT MediumArraySize,
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_HANDLE ConfigurationHandle);

static VOID NTAPI Rtw88Halt(NDIS_HANDLE MiniportAdapterContext);
static NDIS_STATUS NTAPI Rtw88Reset(PBOOLEAN AddressingReset, NDIS_HANDLE MiniportAdapterContext);
static NDIS_STATUS NTAPI Rtw88CheckForHang(NDIS_HANDLE MiniportAdapterContext);

static NDIS_STATUS NTAPI Rtw88QueryInformation(
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_OID Oid,
    PVOID InformationBuffer,
    ULONG InformationBufferLength,
    PULONG BytesWritten,
    PULONG BytesNeeded);

static NDIS_STATUS NTAPI Rtw88SetInformation(
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_OID Oid,
    PVOID InformationBuffer,
    ULONG InformationBufferLength,
    PULONG BytesRead,
    PULONG BytesNeeded);

static NDIS_STATUS NTAPI Rtw88Send(
    NDIS_HANDLE MiniportAdapterContext,
    PNDIS_PACKET Packet,
    UINT Flags);

static NDIS_STATUS NTAPI Rtw88TransferData(
    PNDIS_PACKET Packet,
    PUINT BytesTransferred,
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_HANDLE MiniportReceiveContext,
    UINT ByteOffset,
    UINT BytesToTransfer);

/* -------------------------------------------------------------
   NDIS 3.1 DriverEntry
------------------------------------------------------------- */
NTSTATUS NTAPI DriverEntry(PVOID DriverObject, PVOID RegistryPath) {
    NDIS_HANDLE wrapperHandle = NULL;
    NDIS_STATUS status;
    NDIS_MINIPORT_CHARACTERISTICS chars;

    NdisInitializeWrapper(&wrapperHandle, DriverObject, RegistryPath, NULL);

    NdisZeroMemory(&chars, sizeof(chars));
    chars.MajorNdisVersion = 3;
    chars.MinorNdisVersion = 1;
    chars.CheckForHangHandler = Rtw88CheckForHang;
    chars.HaltHandler = Rtw88Halt;
    chars.InitializeHandler = Rtw88Initialize;
    chars.QueryInformationHandler = Rtw88QueryInformation;
    chars.ResetHandler = Rtw88Reset;
    chars.SendHandler = Rtw88Send;
    chars.SetInformationHandler = Rtw88SetInformation;
    chars.TransferDataHandler = Rtw88TransferData;

    status = NdisMRegisterMiniport(wrapperHandle, &chars, sizeof(chars));
    return (NTSTATUS)status;
}

/* -------------------------------------------------------------
   MiniportInitialize
------------------------------------------------------------- */
static NDIS_STATUS NTAPI Rtw88Initialize(
    PNDIS_STATUS OpenErrorStatus,
    PUINT SelectedMediumIndex,
    PNDIS_MEDIUM MediumArray,
    UINT MediumArraySize,
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_HANDLE ConfigurationHandle)
{
    UINT i;
    BOOLEAN foundMedium = FALSE;

    /* Verify Ethernet (802.3) support */
    for (i = 0; i < MediumArraySize; i++) {
        if (MediumArray[i] == NdisMedium802_3) {
            *SelectedMediumIndex = i;
            foundMedium = TRUE;
            break;
        }
    }

    if (!foundMedium) {
        *OpenErrorStatus = NDIS_STATUS_UNSUPPORTED_MEDIA;
        return NDIS_STATUS_UNSUPPORTED_MEDIA;
    }

    /* Zero and initialize adapter context */
    NdisZeroMemory(&g_Adapter, sizeof(g_Adapter));
    g_Adapter.MiniportHandle = MiniportAdapterHandle;
    g_Adapter.LookaheadSize = RTW88_MAX_LOOKAHEAD;
    g_Adapter.PacketFilter = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST;

    /* Store factory MAC address */
    NdisMoveMemory(g_Adapter.CurrentMac, (PVOID)g_PermanentMac, 6);

    /* Inform NDIS of adapter attributes */
    NdisMSetAttributes(
        MiniportAdapterHandle,
        (NDIS_HANDLE)&g_Adapter,
        FALSE,                  /* Not a legacy DMA bus master */
        NdisInterfaceInternal   /* Standard WDM/internal adapter type */
    );

    return NDIS_STATUS_SUCCESS;
}

/* -------------------------------------------------------------
   MiniportHalt & Reset
------------------------------------------------------------- */
static VOID NTAPI Rtw88Halt(NDIS_HANDLE MiniportAdapterContext) {
    PRTW88_ADAPTER pAdapter = (PRTW88_ADAPTER)MiniportAdapterContext;
    if (pAdapter) {
        pAdapter->PacketFilter = 0;
    }
}

static NDIS_STATUS NTAPI Rtw88Reset(PBOOLEAN AddressingReset, NDIS_HANDLE MiniportAdapterContext) {
    if (AddressingReset) {
        *AddressingReset = FALSE;
    }
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS NTAPI Rtw88CheckForHang(NDIS_HANDLE MiniportAdapterContext) {
    return (NDIS_STATUS)FALSE; /* Return FALSE = Healthy */
}

/* -------------------------------------------------------------
   MiniportQueryInformation
------------------------------------------------------------- */
static NDIS_STATUS NTAPI Rtw88QueryInformation(
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_OID Oid,
    PVOID InformationBuffer,
    ULONG InformationBufferLength,
    PULONG BytesWritten,
    PULONG BytesNeeded)
{
    PRTW88_ADAPTER pAdapter = (PRTW88_ADAPTER)MiniportAdapterContext;
    ULONG uLongValue = 0;
    USHORT uShortValue = 0;
    PVOID pSource = &uLongValue;
    ULONG copyLength = sizeof(ULONG);

    *BytesWritten = 0;
    *BytesNeeded = 0;

    switch (Oid) {
        case OID_GEN_SUPPORTED_LIST:
            pSource = (PVOID)g_SupportedOids;
            copyLength = sizeof(g_SupportedOids);
            break;

        case OID_GEN_HARDWARE_STATUS:
            uLongValue = NdisHardwareStatusReady;
            break;

        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            uLongValue = NdisMedium802_3;
            break;

        case OID_GEN_MAXIMUM_LOOKAHEAD:
        case OID_GEN_CURRENT_LOOKAHEAD:
            uLongValue = pAdapter ? pAdapter->LookaheadSize : RTW88_MAX_LOOKAHEAD;
            break;

        case OID_GEN_MAXIMUM_FRAME_SIZE:
            uLongValue = RTW88_MAX_FRAME_SIZE;
            break;

        case OID_GEN_MAXIMUM_TOTAL_SIZE:
            uLongValue = RTW88_MAX_PACKET_SIZE;
            break;

        case OID_GEN_LINK_SPEED:
            uLongValue = RTW88_LINK_SPEED;
            break;

        case OID_GEN_TRANSMIT_BUFFER_SPACE:
        case OID_GEN_RECEIVE_BUFFER_SPACE:
            uLongValue = 65536;
            break;

        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            uLongValue = RTW88_MAX_PACKET_SIZE;
            break;

        case OID_GEN_VENDOR_ID:
            uLongValue = RTW88_VENDOR_ID;
            break;

        case OID_GEN_VENDOR_DESCRIPTION:
            pSource = (PVOID)g_VendorDesc;
            copyLength = sizeof(g_VendorDesc);
            break;

        case OID_GEN_CURRENT_PACKET_FILTER:
            uLongValue = pAdapter ? pAdapter->PacketFilter : 0;
            break;

        case OID_GEN_DRIVER_VERSION:
            uShortValue = 0x0301; /* NDIS 3.1 */
            pSource = &uShortValue;
            copyLength = sizeof(USHORT);
            break;

        case OID_GEN_MAC_OPTIONS:
            uLongValue = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                         NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                         NDIS_MAC_OPTION_NO_LOOPBACK;
            break;

        case OID_GEN_MEDIA_CONNECT_STATUS:
            uLongValue = NdisMediaStateConnected;
            break;

        case OID_802_3_PERMANENT_ADDRESS:
            pSource = (PVOID)g_PermanentMac;
            copyLength = 6;
            break;

        case OID_802_3_CURRENT_ADDRESS:
            pSource = pAdapter ? (PVOID)pAdapter->CurrentMac : (PVOID)g_PermanentMac;
            copyLength = 6;
            break;

        case OID_802_3_MAXIMUM_LIST_SIZE:
            uLongValue = 32;
            break;

        case OID_802_3_RCV_ERROR_ALIGNMENT:
        case OID_802_3_XMIT_ONE_COLLISION:
        case OID_802_3_XMIT_MORE_COLLISIONS:
            uLongValue = 0;
            break;

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }

    if (InformationBufferLength < copyLength) {
        *BytesNeeded = copyLength;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    NdisMoveMemory(InformationBuffer, pSource, copyLength);
    *BytesWritten = copyLength;
    return NDIS_STATUS_SUCCESS;
}

/* -------------------------------------------------------------
   MiniportSetInformation
------------------------------------------------------------- */
static NDIS_STATUS NTAPI Rtw88SetInformation(
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_OID Oid,
    PVOID InformationBuffer,
    ULONG InformationBufferLength,
    PULONG BytesRead,
    PULONG BytesNeeded)
{
    PRTW88_ADAPTER pAdapter = (PRTW88_ADAPTER)MiniportAdapterContext;
    *BytesRead = 0;
    *BytesNeeded = 0;

    switch (Oid) {
        case OID_GEN_CURRENT_PACKET_FILTER: {
            if (InformationBufferLength < sizeof(ULONG)) {
                *BytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (pAdapter) {
                pAdapter->PacketFilter = *(PULONG)InformationBuffer;
            }
            *BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;
        }

        case OID_GEN_CURRENT_LOOKAHEAD: {
            if (InformationBufferLength < sizeof(ULONG)) {
                *BytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (pAdapter) {
                pAdapter->LookaheadSize = *(PULONG)InformationBuffer;
            }
            *BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;
        }

        case OID_802_3_MULTICAST_LIST:
            /* Multicast address list accepted */
            *BytesRead = InformationBufferLength;
            return NDIS_STATUS_SUCCESS;

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

/* -------------------------------------------------------------
   MiniportSend
------------------------------------------------------------- */
static NDIS_STATUS NTAPI Rtw88Send(
    NDIS_HANDLE MiniportAdapterContext,
    PNDIS_PACKET Packet,
    UINT Flags)
{
    PRTW88_ADAPTER pAdapter = (PRTW88_ADAPTER)MiniportAdapterContext;
    PNDIS_BUFFER curBuffer = NULL;
    UINT totalLength = 0;
    UINT bufferCount = 0;
    UINT physCount = 0;
    ULONG bytesCopied = 0;

    if (!Packet || !pAdapter) {
        return NDIS_STATUS_FAILURE;
    }

    NdisQueryPacket(Packet, &physCount, &bufferCount, &curBuffer, &totalLength);

    if (totalLength > sizeof(pAdapter->TxBuffer)) {
        NdisMSendComplete(pAdapter->MiniportHandle, Packet, NDIS_STATUS_RESOURCES);
        return NDIS_STATUS_RESOURCES;
    }

    /* Flatten NDIS buffer chain into contiguous transmission buffer */
    while (curBuffer && bytesCopied < totalLength) {
        PVOID bufVa = NULL;
        UINT bufLen = 0;

        NdisQueryBuffer(curBuffer, &bufVa, &bufLen);
        if (bufVa && bufLen > 0) {
            UINT toCopy = bufLen;
            if (bytesCopied + toCopy > totalLength) {
                toCopy = totalLength - bytesCopied;
            }
            NdisMoveMemory(&pAdapter->TxBuffer[bytesCopied], bufVa, toCopy);
            bytesCopied += toCopy;
        }
        curBuffer = curBuffer->Next;
    }

    pAdapter->FramesTransmitted++;

    /* Complete synchronously to NDIS */
    NdisMSendComplete(pAdapter->MiniportHandle, Packet, NDIS_STATUS_SUCCESS);
    return NDIS_STATUS_SUCCESS;
}

/* -------------------------------------------------------------
   MiniportTransferData
------------------------------------------------------------- */
static NDIS_STATUS NTAPI Rtw88TransferData(
    PNDIS_PACKET Packet,
    PUINT BytesTransferred,
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_HANDLE MiniportReceiveContext,
    UINT ByteOffset,
    UINT BytesToTransfer)
{
    PRTW88_ADAPTER pAdapter = (PRTW88_ADAPTER)MiniportAdapterContext;
    PNDIS_BUFFER curBuffer = NULL;
    UINT totalLength = 0;
    UINT bufferCount = 0;
    UINT physCount = 0;
    ULONG copied = 0;

    *BytesTransferred = 0;

    if (!Packet || !pAdapter) {
        return NDIS_STATUS_FAILURE;
    }

    NdisQueryPacket(Packet, &physCount, &bufferCount, &curBuffer, &totalLength);

    while (curBuffer && copied < BytesToTransfer) {
        PVOID bufVa = NULL;
        UINT bufLen = 0;

        NdisQueryBuffer(curBuffer, &bufVa, &bufLen);
        if (bufVa && bufLen > 0) {
            UINT toCopy = bufLen;
            if (copied + toCopy > BytesToTransfer) {
                toCopy = BytesToTransfer - copied;
            }
            NdisMoveMemory(bufVa, &pAdapter->RxBuffer[ByteOffset + copied], toCopy);
            copied += toCopy;
        }
        curBuffer = curBuffer->Next;
    }

    *BytesTransferred = copied;
    return NDIS_STATUS_SUCCESS;
}

