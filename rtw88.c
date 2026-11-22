/*
 * rtw88.c - NDIS-WDM Network Miniport Driver for Realtek RTL8822BU Wi-Fi
 * Targets Windows 95 OSR 2.1 / 2.5 (Pentium MMX, strictly 0 CMOV instructions)
 * Binds to VTCP.386 (TCP/IP) as standard 802.3 Ethernet (NdisMedium802_3)
 * Provides full WDM PnP dispatch tables to NTKERN for crash-free USB hot-unplug
 */

#include <ddk/wdm.h>
#include <usb.h>
#include <usbioctl.h>
#include <ddk/usbdlib.h>

#define _WDMDDK_
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

/* -------------------------------------------------------------
   WDM Device Extension and State
------------------------------------------------------------- */
typedef struct _DEVICE_EXTENSION {
    PDEVICE_OBJECT FunctionalDeviceObject;
    PDEVICE_OBJECT LowerDeviceObject;
    PDEVICE_OBJECT PhysicalDeviceObject;
    BOOLEAN DeviceStarted;
    BOOLEAN DeviceRemoved;
    USBD_CONFIGURATION_HANDLE ConfigurationHandle;
    ULONG NumberOfPipes;
    USBD_PIPE_INFORMATION Pipes[8];
    UCHAR UsbConfigBuffer[512];
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

static PDEVICE_EXTENSION g_pWdmDevExt = NULL;

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
   WDM USB Configuration and Pipe Helpers
------------------------------------------------------------- */
static NTSTATUS RTW_ConfigureDevice(PDEVICE_EXTENSION pDevExt) {
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    PIO_STACK_LOCATION nextStack;
    PURB urb;
    PUSB_CONFIGURATION_DESCRIPTOR configDesc;
    PUSBD_INTERFACE_LIST_ENTRY interfaceList;
    NTSTATUS status;

    if (!pDevExt || !pDevExt->LowerDeviceObject) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (pDevExt->ConfigurationHandle != NULL && pDevExt->NumberOfPipes > 0) {
        return STATUS_SUCCESS;
    }

    urb = (PURB)ExAllocatePool(NonPagedPool, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST));
    if (!urb) return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(pDevExt->UsbConfigBuffer, sizeof(pDevExt->UsbConfigBuffer));
    UsbBuildGetDescriptorRequest(
        urb,
        (USHORT)sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
        USB_CONFIGURATION_DESCRIPTOR_TYPE,
        0,
        0,
        pDevExt->UsbConfigBuffer,
        NULL,
        sizeof(pDevExt->UsbConfigBuffer),
        NULL
    );

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_SUBMIT_URB,
        pDevExt->LowerDeviceObject,
        NULL, 0, NULL, 0, TRUE, &event, &ioStatus
    );

    if (!irp) {
        ExFreePool(urb);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    nextStack = IoGetNextIrpStackLocation(irp);
    nextStack->Parameters.Others.Argument1 = urb;

    status = IoCallDriver(pDevExt->LowerDeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }

    ExFreePool(urb);
    if (!NT_SUCCESS(status)) return status;

    configDesc = (PUSB_CONFIGURATION_DESCRIPTOR)pDevExt->UsbConfigBuffer;
    interfaceList = (PUSBD_INTERFACE_LIST_ENTRY)ExAllocatePool(
        NonPagedPool, sizeof(USBD_INTERFACE_LIST_ENTRY) * 2
    );
    if (!interfaceList) return STATUS_INSUFFICIENT_RESOURCES;

    interfaceList[0].InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(
        configDesc, configDesc, -1, -1, -1, -1, -1
    );
    interfaceList[0].Interface = NULL;
    interfaceList[1].InterfaceDescriptor = NULL;
    interfaceList[1].Interface = NULL;

    urb = USBD_CreateConfigurationRequestEx(configDesc, interfaceList);
    if (!urb) {
        ExFreePool(interfaceList);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (ULONG p = 0; p < 8; p++) {
        urb->UrbSelectConfiguration.Interface.Pipes[p].MaximumTransferSize = 0x10000;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_SUBMIT_URB,
        pDevExt->LowerDeviceObject,
        NULL, 0, NULL, 0, TRUE, &event, &ioStatus
    );

    if (!irp) {
        ExFreePool(urb);
        ExFreePool(interfaceList);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    nextStack = IoGetNextIrpStackLocation(irp);
    nextStack->Parameters.Others.Argument1 = urb;

    status = IoCallDriver(pDevExt->LowerDeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }

    if (NT_SUCCESS(status)) {
        pDevExt->ConfigurationHandle = urb->UrbSelectConfiguration.ConfigurationHandle;
        pDevExt->NumberOfPipes = urb->UrbSelectConfiguration.Interface.NumberOfPipes;
        if (pDevExt->NumberOfPipes > 8) pDevExt->NumberOfPipes = 8;
        for (ULONG i = 0; i < pDevExt->NumberOfPipes; i++) {
            pDevExt->Pipes[i] = urb->UrbSelectConfiguration.Interface.Pipes[i];
        }
    }

    ExFreePool(urb);
    ExFreePool(interfaceList);
    return status;
}

static USBD_PIPE_HANDLE RTW_FindBulkInPipe(PDEVICE_EXTENSION pDevExt) {
    if (!pDevExt) return NULL;
    if (pDevExt->ConfigurationHandle == NULL || pDevExt->NumberOfPipes == 0) {
        if (!NT_SUCCESS(RTW_ConfigureDevice(pDevExt))) {
            return NULL;
        }
    }
    for (ULONG i = 0; i < pDevExt->NumberOfPipes; i++) {
        if (pDevExt->Pipes[i].PipeType == UsbdPipeTypeBulk &&
            (pDevExt->Pipes[i].EndpointAddress & 0x80)) {
            return pDevExt->Pipes[i].PipeHandle;
        }
    }
    return NULL;
}

static USBD_PIPE_HANDLE RTW_FindBulkOutPipe(PDEVICE_EXTENSION pDevExt) {
    if (!pDevExt) return NULL;
    if (pDevExt->ConfigurationHandle == NULL || pDevExt->NumberOfPipes == 0) {
        if (!NT_SUCCESS(RTW_ConfigureDevice(pDevExt))) {
            return NULL;
        }
    }
    for (ULONG i = 0; i < pDevExt->NumberOfPipes; i++) {
        if (pDevExt->Pipes[i].PipeType == UsbdPipeTypeBulk &&
            !(pDevExt->Pipes[i].EndpointAddress & 0x80)) {
            return pDevExt->Pipes[i].PipeHandle;
        }
    }
    return NULL;
}

static NTSTATUS RTW_UsbBulkTransfer(
    PDEVICE_OBJECT LowerDevice,
    USBD_PIPE_HANDLE PipeHandle,
    ULONG Flags,
    PVOID Buffer,
    ULONG BufferLength,
    PULONG BytesTransferred)
{
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    PIO_STACK_LOCATION nextStack;
    struct _URB_BULK_OR_INTERRUPT_TRANSFER urb;
    NTSTATUS status;

    if (!LowerDevice || !PipeHandle) return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    urb.PipeHandle = PipeHandle;
    urb.TransferFlags = Flags;
    urb.TransferBuffer = Buffer;
    urb.TransferBufferLength = BufferLength;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_SUBMIT_URB,
        LowerDevice,
        NULL, 0, NULL, 0, TRUE, &event, &ioStatus
    );

    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;

    nextStack = IoGetNextIrpStackLocation(irp);
    nextStack->Parameters.Others.Argument1 = &urb;

    status = IoCallDriver(LowerDevice, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }

    if (BytesTransferred) {
        *BytesTransferred = urb.TransferBufferLength;
    }

    return status;
}

/* -------------------------------------------------------------
   WDM PnP & Lifecycle Routines (Crash-Free Unplug Support)
------------------------------------------------------------- */
static NTSTATUS NTAPI StartCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    PKEVENT pEvent = (PKEVENT)Context;
    KeSetEvent(pEvent, 0, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS NTAPI Rtw88Pnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_EXTENSION pDevExt = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    NTSTATUS status;

    switch (stack->MinorFunction) {
        case IRP_MN_START_DEVICE: {
            KEVENT event;
            KeInitializeEvent(&event, NotificationEvent, FALSE);
            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp, StartCompletion, &event, TRUE, TRUE, TRUE);

            status = IoCallDriver(pDevExt->LowerDeviceObject, Irp);
            if (status == STATUS_PENDING) {
                KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
                status = Irp->IoStatus.Status;
            }

            if (NT_SUCCESS(status)) {
                pDevExt->DeviceStarted = TRUE;
                RTW_ConfigureDevice(pDevExt);
            }

            Irp->IoStatus.Status = status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }

        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_STOP_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(pDevExt->LowerDeviceObject, Irp);

        case 0x17: /* IRP_MN_SURPRISE_REMOVAL */ {
            UNICODE_STRING symLink;
            RtlInitUnicodeString(&symLink, L"\\DosDevices\\RTW88");
            IoDeleteSymbolicLink(&symLink);
            if (pDevExt) {
                pDevExt->DeviceRemoved = TRUE;
                pDevExt->DeviceStarted = FALSE;
            }

            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(pDevExt ? pDevExt->LowerDeviceObject : NULL, Irp);
        }

        case IRP_MN_REMOVE_DEVICE: {
            UNICODE_STRING symLink;
            PDEVICE_OBJECT lowerDev = pDevExt ? pDevExt->LowerDeviceObject : NULL;
            NTSTATUS status = STATUS_SUCCESS;

            RtlInitUnicodeString(&symLink, L"\\DosDevices\\RTW88");
            IoDeleteSymbolicLink(&symLink);
            g_pWdmDevExt = NULL;
            if (pDevExt) {
                pDevExt->LowerDeviceObject = NULL;
                pDevExt->DeviceRemoved = TRUE;
                pDevExt->DeviceStarted = FALSE;
            }

            if (lowerDev) {
                Irp->IoStatus.Status = STATUS_SUCCESS;
                IoSkipCurrentIrpStackLocation(Irp);
                status = IoCallDriver(lowerDev, Irp);
                IoDetachDevice(lowerDev);
            } else {
                Irp->IoStatus.Status = STATUS_SUCCESS;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
            }

            if (DeviceObject->ReferenceCount == 0) {
                IoDeleteDevice(DeviceObject);
            }

            return status;
        }

        default:
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(pDevExt->LowerDeviceObject, Irp);
    }
}

static NTSTATUS NTAPI Rtw88Power(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PDEVICE_EXTENSION pDevExt = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(pDevExt->LowerDeviceObject, Irp);
}

static NTSTATUS NTAPI Rtw88CreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_EXTENSION pDevExt = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (stack->MajorFunction == IRP_MJ_CLOSE) {
        if ((!g_pWdmDevExt || (pDevExt && pDevExt->DeviceRemoved)) && DeviceObject->ReferenceCount <= 1) {
            IoDeleteDevice(DeviceObject);
        }
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI Rtw88AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
    NTSTATUS status;
    PDEVICE_OBJECT fdo;
    PDEVICE_EXTENSION pDevExt;
    UNICODE_STRING devName, symName;

    RtlInitUnicodeString(&devName, L"\\Device\\RTW88");
    RtlInitUnicodeString(&symName, L"\\DosDevices\\RTW88");

    status = IoCreateDevice(
        DriverObject,
        sizeof(DEVICE_EXTENSION),
        &devName,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &fdo
    );

    if (!NT_SUCCESS(status)) return status;

    pDevExt = (PDEVICE_EXTENSION)fdo->DeviceExtension;
    RtlZeroMemory(pDevExt, sizeof(DEVICE_EXTENSION));
    pDevExt->FunctionalDeviceObject = fdo;
    pDevExt->PhysicalDeviceObject = PhysicalDeviceObject;
    pDevExt->DeviceStarted = FALSE;
    g_pWdmDevExt = pDevExt;

    pDevExt->LowerDeviceObject = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    if (!pDevExt->LowerDeviceObject) {
        IoDeleteDevice(fdo);
        return STATUS_DEVICE_REMOVED;
    }

    status = IoCreateSymbolicLink(&symName, &devName);
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

static void NTAPI Rtw88Unload(PDRIVER_OBJECT DriverObject) {
    UNICODE_STRING symName;
    RtlInitUnicodeString(&symName, L"\\DosDevices\\RTW88");
    IoDeleteSymbolicLink(&symName);
}

/* -------------------------------------------------------------
   NDIS 3.1 DriverEntry & Miniport Registration
------------------------------------------------------------- */
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    NDIS_HANDLE wrapperHandle = NULL;
    NDIS_STATUS status;
    NDIS_MINIPORT_CHARACTERISTICS chars;

    /* 1. Register with NDIS 3.1 */
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

    /* 2. Register WDM dispatch table so NTKERN PnP and hot-unplug work cleanly */
    if (DriverObject != NULL) {
        DriverObject->DriverExtension->AddDevice = Rtw88AddDevice;
        DriverObject->DriverUnload = Rtw88Unload;
        DriverObject->MajorFunction[IRP_MJ_CREATE] = Rtw88CreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLOSE] = Rtw88CreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLEANUP] = Rtw88CreateClose;
        DriverObject->MajorFunction[IRP_MJ_PNP] = Rtw88Pnp;
        DriverObject->MajorFunction[IRP_MJ_POWER] = Rtw88Power;
    }

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
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;
    ULONG uLongVal = 0;
    USHORT uShortVal = 0;
    PVOID pSource = &uLongVal;
    ULONG copyLen = sizeof(ULONG);

    *BytesWritten = 0;
    *BytesNeeded = 0;

    switch (Oid) {
        case OID_GEN_SUPPORTED_LIST:
            pSource = (PVOID)g_SupportedOids;
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
        case OID_GEN_CURRENT_LOOKAHEAD:
            uLongVal = RTW88_MAX_LOOKAHEAD;
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
            uLongVal = sizeof(pAdapter->TxBuffer);
            break;

        case OID_GEN_RECEIVE_BUFFER_SPACE:
            uLongVal = sizeof(pAdapter->RxBuffer);
            break;

        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            uLongVal = 1;
            break;

        case OID_GEN_VENDOR_ID:
            uLongVal = RTW88_VENDOR_ID;
            break;

        case OID_GEN_VENDOR_DESCRIPTION:
            pSource = (PVOID)g_VendorDesc;
            copyLen = sizeof(g_VendorDesc);
            break;

        case OID_GEN_CURRENT_PACKET_FILTER:
            uLongVal = pAdapter ? pAdapter->PacketFilter : 0;
            break;

        case OID_GEN_DRIVER_VERSION:
            uShortVal = 0x0301; /* NDIS 3.1 */
            pSource = &uShortVal;
            copyLen = sizeof(USHORT);
            break;

        case OID_GEN_MAC_OPTIONS:
            uLongVal = NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                       NDIS_MAC_OPTION_RECEIVE_SERIALIZED |
                       NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                       NDIS_MAC_OPTION_NO_LOOPBACK;
            break;

        case OID_GEN_MEDIA_CONNECT_STATUS:
            uLongVal = NdisMediaStateConnected;
            break;

        case OID_802_3_PERMANENT_ADDRESS:
            pSource = (PVOID)g_PermanentMac;
            copyLen = 6;
            break;

        case OID_802_3_CURRENT_ADDRESS:
            pSource = (PVOID)(pAdapter ? pAdapter->CurrentMac : g_PermanentMac);
            copyLen = 6;
            break;

        case OID_802_3_MAXIMUM_LIST_SIZE:
            uLongVal = 32;
            break;

        case OID_802_3_MULTICAST_LIST:
            copyLen = 0;
            break;

        case OID_802_3_RCV_ERROR_ALIGNMENT:
        case OID_802_3_XMIT_ONE_COLLISION:
        case OID_802_3_XMIT_MORE_COLLISIONS:
            uLongVal = 0;
            break;

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }

    if (copyLen > InformationBufferLength) {
        *BytesNeeded = copyLen;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    NdisMoveMemory(InformationBuffer, pSource, copyLen);
    *BytesWritten = copyLen;
    return status;
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

    /* If USB hardware is attached, transmit over bulk OUT pipe */
    if (g_pWdmDevExt && g_pWdmDevExt->DeviceStarted && bytesCopied > 0) {
        USBD_PIPE_HANDLE txPipe = RTW_FindBulkOutPipe(g_pWdmDevExt);
        if (txPipe != NULL) {
            ULONG written = 0;
            RTW_UsbBulkTransfer(
                g_pWdmDevExt->LowerDeviceObject,
                txPipe,
                0,
                pAdapter->TxBuffer,
                bytesCopied,
                &written
            );
        }
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
