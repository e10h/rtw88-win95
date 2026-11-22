/*
 * ndis31.h - Self-contained NDIS 3.1 Header for Windows 95
 *
 * Realtek RTL8822BU NDIS Miniport Driver (RTW88)
 * Targets Pentium MMX (strictly 0 CMOV instructions)
 */

#ifndef _NDIS31_H_
#define _NDIS31_H_

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef ULONG NDIS_STATUS, *PNDIS_STATUS;
typedef PVOID NDIS_HANDLE, *PNDIS_HANDLE;
typedef ULONG NDIS_OID, *PNDIS_OID;

/* NDIS Status Codes */
#define NDIS_STATUS_SUCCESS                     ((NDIS_STATUS)0x00000000L)
#define NDIS_STATUS_PENDING                     ((NDIS_STATUS)0x00000103L)
#define NDIS_STATUS_NOT_RECOGNIZED              ((NDIS_STATUS)0x00010001L)
#define NDIS_STATUS_NOT_COPIED                  ((NDIS_STATUS)0x00010002L)
#define NDIS_STATUS_NOT_ACCEPTED                ((NDIS_STATUS)0x00010003L)
#define NDIS_STATUS_ONLINE                      ((NDIS_STATUS)0x40010003L)
#define NDIS_STATUS_RESET_START                 ((NDIS_STATUS)0x40010004L)
#define NDIS_STATUS_RESET_END                   ((NDIS_STATUS)0x40010005L)
#define NDIS_STATUS_MEDIA_CONNECT               ((NDIS_STATUS)0x4001000BL)
#define NDIS_STATUS_MEDIA_DISCONNECT            ((NDIS_STATUS)0x4001000CL)
#define NDIS_STATUS_HARDWARE_ERROR              ((NDIS_STATUS)0xC0010001L)
#define NDIS_STATUS_NOT_SUPPORTED               ((NDIS_STATUS)0xC00000BBL)
#define NDIS_STATUS_BUFFER_TOO_SHORT            ((NDIS_STATUS)0xC0010016L)
#define NDIS_STATUS_INVALID_LENGTH              ((NDIS_STATUS)0xC0010014L)
#define NDIS_STATUS_INVALID_DATA                ((NDIS_STATUS)0xC0010015L)
#define NDIS_STATUS_INVALID_OID                 ((NDIS_STATUS)0xC0010017L)
#define NDIS_STATUS_ADAPTER_NOT_FOUND           ((NDIS_STATUS)0xC001000BL)
#define NDIS_STATUS_OPEN_FAILED                 ((NDIS_STATUS)0xC001000CL)
#define NDIS_STATUS_DEVICE_FAILED               ((NDIS_STATUS)0xC001000DL)
#define NDIS_STATUS_UNSUPPORTED_MEDIA           ((NDIS_STATUS)0xC001000FL)
#define NDIS_STATUS_CLOSING                     ((NDIS_STATUS)0xC0010011L)
#define NDIS_STATUS_RESET_IN_PROGRESS           ((NDIS_STATUS)0xC0010013L)
#define NDIS_STATUS_RESOURCES                   ((NDIS_STATUS)0xC000009AL)
#define NDIS_STATUS_FAILURE                     ((NDIS_STATUS)0xC0000001L)

/* Medium Types */
typedef enum _NDIS_MEDIUM {
    NdisMedium802_3,
    NdisMedium802_5,
    NdisMediumFddi,
    NdisMediumWan,
    NdisMediumLocalTalk,
    NdisMediumDix,
    NdisMediumArcnetRaw,
    NdisMediumArcnet878_2,
    NdisMediumAtm,
    NdisMediumWirelessWan,
    NdisMediumIrda,
    NdisMediumBpc,
    NdisMediumCoWan,
    NdisMedium1394,
    NdisMediumMax
} NDIS_MEDIUM, *PNDIS_MEDIUM;

/* Hardware Status */
typedef enum _NDIS_HARDWARE_STATUS {
    NdisHardwareStatusReady,
    NdisHardwareStatusInitializing,
    NdisHardwareStatusReset,
    NdisHardwareStatusClosing,
    NdisHardwareStatusNotReady
} NDIS_HARDWARE_STATUS, *PNDIS_HARDWARE_STATUS;

/* Media State */
typedef enum _NDIS_MEDIA_STATE {
    NdisMediaStateConnected,
    NdisMediaStateDisconnected
} NDIS_MEDIA_STATE, *PNDIS_MEDIA_STATE;

/* Interface Types */
typedef enum _NDIS_INTERFACE_TYPE {
    NdisInterfaceInternal = 0,
    NdisInterfaceIsa,
    NdisInterfaceEisa,
    NdisInterfaceMca,
    NdisInterfaceTurboChannel,
    NdisInterfacePci,
    NdisInterfacePcMcia,
    NdisInterfaceCBus,
    NdisInterfaceMPIBus,
    NdisInterfaceMPSABus,
    NdisInterfaceProcessorInternal,
    NdisInterfaceInternalPowerBus,
    NdisInterfacePNPISABus,
    NdisInterfacePNPBus,
    NdisInterfaceUSB,
    NdisMaximumInterfaceType
} NDIS_INTERFACE_TYPE, *PNDIS_INTERFACE_TYPE;

/* Packet Filter Bits */
#define NDIS_PACKET_TYPE_DIRECTED               0x00000001
#define NDIS_PACKET_TYPE_MULTICAST              0x00000002
#define NDIS_PACKET_TYPE_ALL_MULTICAST          0x00000004
#define NDIS_PACKET_TYPE_BROADCAST              0x00000008
#define NDIS_PACKET_TYPE_SOURCE_ROUTING         0x00000010
#define NDIS_PACKET_TYPE_PROMISCUOUS            0x00000020
#define NDIS_PACKET_TYPE_ALL_LOCAL              0x00000080

/* MAC Options */
#define NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA     0x00000001
#define NDIS_MAC_OPTION_RECEIVE_SERIALIZED      0x00000002
#define NDIS_MAC_OPTION_TRANSFERS_NOT_PEND      0x00000004
#define NDIS_MAC_OPTION_NO_LOOPBACK             0x00000008
#define NDIS_MAC_OPTION_FULL_DUPLEX             0x00000010

/* General OIDs */
#define OID_GEN_SUPPORTED_LIST                  0x00010101
#define OID_GEN_HARDWARE_STATUS                 0x00010102
#define OID_GEN_MEDIA_SUPPORTED                 0x00010103
#define OID_GEN_MEDIA_IN_USE                    0x00010104
#define OID_GEN_MAXIMUM_LOOKAHEAD               0x00010105
#define OID_GEN_MAXIMUM_FRAME_SIZE              0x00010106
#define OID_GEN_LINK_SPEED                      0x00010107
#define OID_GEN_TRANSMIT_BUFFER_SPACE           0x00010108
#define OID_GEN_RECEIVE_BUFFER_SPACE            0x00010109
#define OID_GEN_TRANSMIT_BLOCK_SIZE             0x0001010A
#define OID_GEN_RECEIVE_BLOCK_SIZE              0x0001010B
#define OID_GEN_VENDOR_ID                       0x0001010C
#define OID_GEN_VENDOR_DESCRIPTION              0x0001010D
#define OID_GEN_CURRENT_PACKET_FILTER           0x0001010E
#define OID_GEN_CURRENT_LOOKAHEAD               0x0001010F
#define OID_GEN_DRIVER_VERSION                  0x00010110
#define OID_GEN_MAXIMUM_TOTAL_SIZE              0x00010111
#define OID_GEN_PROTOCOL_OPTIONS                0x00010112
#define OID_GEN_MAC_OPTIONS                     0x00010113
#define OID_GEN_MEDIA_CONNECT_STATUS            0x00010114
#define OID_GEN_MAXIMUM_SEND_PACKETS            0x00010115

/* 802.3 Ethernet Specific OIDs */
#define OID_802_3_PERMANENT_ADDRESS             0x01010101
#define OID_802_3_CURRENT_ADDRESS               0x01010102
#define OID_802_3_MULTICAST_LIST                0x01010103
#define OID_802_3_MAXIMUM_LIST_SIZE             0x01010104
#define OID_802_3_MAC_OPTIONS                   0x01010105
#define OID_802_3_RCV_ERROR_ALIGNMENT           0x01020101
#define OID_802_3_XMIT_ONE_COLLISION            0x01020102
#define OID_802_3_XMIT_MORE_COLLISIONS          0x01020103
#define OID_802_3_RCV_OVERRUN                   0x01020204

/* Opaque NDIS Buffer and Packet Types */
typedef struct _NDIS_BUFFER {
    struct _NDIS_BUFFER *Next;
    PVOID VirtualAddress;
    PVOID Process;
    ULONG ByteCount;
    ULONG ByteOffset;
} NDIS_BUFFER, *PNDIS_BUFFER;

#define NdisGetNextBuffer(CurrentBuffer, NextBuffer) \
    *(NextBuffer) = ((CurrentBuffer) ? (CurrentBuffer)->Next : NULL)

typedef struct _NDIS_PACKET_PRIVATE {
    UINT PhysicalMappingCount;
    ULONG TotalLength;
    PNDIS_BUFFER Head;
    PNDIS_BUFFER Tail;
    PVOID Pool;
    UINT Count;
    ULONG Flags;
    BOOLEAN ValidCounts;
    UCHAR NdisPacketFlags;
    USHORT NdisPacketOobOffset;
} NDIS_PACKET_PRIVATE, *PNDIS_PACKET_PRIVATE;

typedef struct _NDIS_PACKET {
    NDIS_PACKET_PRIVATE Private;
    union {
        struct {
            UCHAR MiniportReserved[16];
            UCHAR WrapperReserved[8];
        };
        struct {
            UCHAR MiniportReservedEx[16];
            UCHAR WrapperReservedEx[8];
        };
        struct {
            UCHAR MacReserved[16];
        };
    };
    ULONG_PTR ProtocolReserved[1];
} NDIS_PACKET, *PNDIS_PACKET;

/* Forward declare handler signatures */
typedef NDIS_STATUS (NTAPI *W_CHECK_FOR_HANG_HANDLER)(
    NDIS_HANDLE MiniportAdapterContext);

typedef VOID (NTAPI *W_DISABLE_INTERRUPT_HANDLER)(
    NDIS_HANDLE MiniportAdapterContext);

typedef VOID (NTAPI *W_ENABLE_INTERRUPT_HANDLER)(
    NDIS_HANDLE MiniportAdapterContext);

typedef VOID (NTAPI *W_HALT_HANDLER)(
    NDIS_HANDLE MiniportAdapterContext);

typedef VOID (NTAPI *W_HANDLE_INTERRUPT_HANDLER)(
    NDIS_HANDLE MiniportAdapterContext);

typedef NDIS_STATUS (NTAPI *W_INITIALIZE_HANDLER)(
    PNDIS_STATUS OpenErrorStatus,
    PUINT SelectedMediumIndex,
    PNDIS_MEDIUM MediumArray,
    UINT MediumArraySize,
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_HANDLE ConfigurationHandle);

typedef VOID (NTAPI *W_ISR_HANDLER)(
    PBOOLEAN InterruptRecognized,
    PBOOLEAN QueueDpc,
    NDIS_HANDLE MiniportAdapterContext);

typedef NDIS_STATUS (NTAPI *W_QUERY_INFORMATION_HANDLER)(
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_OID Oid,
    PVOID InformationBuffer,
    ULONG InformationBufferLength,
    PULONG BytesWritten,
    PULONG BytesNeeded);

typedef NDIS_STATUS (NTAPI *W_RECONFIGURE_HANDLER)(
    PNDIS_STATUS OpenErrorStatus,
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_HANDLE ConfigurationHandle);

typedef NDIS_STATUS (NTAPI *W_RESET_HANDLER)(
    PBOOLEAN AddressingReset,
    NDIS_HANDLE MiniportAdapterContext);

typedef NDIS_STATUS (NTAPI *W_SEND_HANDLER)(
    NDIS_HANDLE MiniportAdapterContext,
    PNDIS_PACKET Packet,
    UINT Flags);

typedef NDIS_STATUS (NTAPI *W_SET_INFORMATION_HANDLER)(
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_OID Oid,
    PVOID InformationBuffer,
    ULONG InformationBufferLength,
    PULONG BytesRead,
    PULONG BytesNeeded);

typedef NDIS_STATUS (NTAPI *W_TRANSFER_DATA_HANDLER)(
    PNDIS_PACKET Packet,
    PUINT BytesTransferred,
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_HANDLE MiniportReceiveContext,
    UINT ByteOffset,
    UINT BytesToTransfer);

typedef struct _NDIS_MINIPORT_CHARACTERISTICS {
    UCHAR MajorNdisVersion;
    UCHAR MinorNdisVersion;
    USHORT Filler;
    UINT Reserved;
    W_CHECK_FOR_HANG_HANDLER CheckForHangHandler;
    W_DISABLE_INTERRUPT_HANDLER DisableInterruptHandler;
    W_ENABLE_INTERRUPT_HANDLER EnableInterruptHandler;
    W_HALT_HANDLER HaltHandler;
    W_HANDLE_INTERRUPT_HANDLER HandleInterruptHandler;
    W_INITIALIZE_HANDLER InitializeHandler;
    W_ISR_HANDLER ISRHandler;
    W_QUERY_INFORMATION_HANDLER QueryInformationHandler;
    W_RECONFIGURE_HANDLER ReconfigureHandler;
    W_RESET_HANDLER ResetHandler;
    W_SEND_HANDLER SendHandler;
    W_SET_INFORMATION_HANDLER SetInformationHandler;
    W_TRANSFER_DATA_HANDLER TransferDataHandler;
} NDIS_MINIPORT_CHARACTERISTICS, *PNDIS_MINIPORT_CHARACTERISTICS;

/* NDIS Wrapper and Miniport Functions (Exported by NDIS.SYS) */
VOID NTAPI NdisInitializeWrapper(
    PNDIS_HANDLE NdisWrapperHandle,
    PVOID SystemSpecific1,
    PVOID SystemSpecific2,
    PVOID SystemSpecific3);

NDIS_STATUS NTAPI NdisMRegisterMiniport(
    NDIS_HANDLE NdisWrapperHandle,
    PNDIS_MINIPORT_CHARACTERISTICS MiniportCharacteristics,
    UINT CharacteristicsLength);

VOID NTAPI NdisMSetAttributes(
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_HANDLE MiniportAdapterContext,
    BOOLEAN BusMaster,
    NDIS_INTERFACE_TYPE AdapterType);

VOID NTAPI NdisMSetAttributesEx(
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_HANDLE MiniportAdapterContext,
    UINT CheckForHangTimeInSeconds,
    ULONG AttributeFlags,
    NDIS_INTERFACE_TYPE AdapterType);

VOID NTAPI NdisMEthIndicateReceive(
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_HANDLE MiniportReceiveContext,
    PCHAR HeaderBuffer,
    UINT HeaderBufferSize,
    PVOID LookaheadBuffer,
    UINT LookaheadBufferSize,
    UINT PacketSize);

VOID NTAPI NdisMEthIndicateReceiveComplete(
    NDIS_HANDLE MiniportAdapterHandle);

VOID NTAPI NdisMSendComplete(
    NDIS_HANDLE MiniportAdapterHandle,
    PNDIS_PACKET Packet,
    NDIS_STATUS Status);

VOID NTAPI NdisMSendResourcesAvailable(
    NDIS_HANDLE MiniportAdapterHandle);

VOID NTAPI NdisMResetComplete(
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_STATUS Status,
    BOOLEAN AddressingReset);

#define NdisQueryPacket(Packet, PhysicalBufferCount, BufferCount, FirstBuffer, TotalPacketLength) \
    do { \
        if (PhysicalBufferCount) *(PhysicalBufferCount) = (Packet)->Private.PhysicalMappingCount; \
        if (BufferCount) *(BufferCount) = (Packet)->Private.Count; \
        if (FirstBuffer) *(FirstBuffer) = (Packet)->Private.Head; \
        if (TotalPacketLength) *(TotalPacketLength) = (Packet)->Private.TotalLength; \
    } while (0)

VOID NTAPI NdisQueryBuffer(
    PNDIS_BUFFER Buffer,
    PVOID *VirtualAddress,
    PUINT Length);

static inline void NdisMoveMemory(PVOID Destination, const PVOID Source, ULONG Length) {
    UCHAR *d = (UCHAR *)Destination;
    const UCHAR *s = (const UCHAR *)Source;
    while (Length--) *d++ = *s++;
}

static inline void NdisZeroMemory(PVOID Destination, ULONG Length) {
    volatile UCHAR *d = (volatile UCHAR *)Destination;
    while (Length--) *d++ = 0;
}

#ifdef __cplusplus
}
#endif

#endif /* _NDIS31_H_ */
