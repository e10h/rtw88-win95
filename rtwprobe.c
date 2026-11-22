#include <ddk/wdm.h>
#include <usb.h>
#include <usbioctl.h>
#include <ddk/usbdlib.h>

#define RTW_USB_CMD_REQ     0x05
#define REG_SYS_FUNC_EN     0x0002
#define REG_SYS_CLK_CTRL    0x0008
#define REG_SYS_CFG1        0x00F0
#define REG_MACID_0610      0x0610
#define REG_MACID_0050      0x0050

#define IOCTL_RTW_PROBE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _RTW_PROBE_DATA {
    USHORT func_en;
    USHORT clk_ctrl;
    ULONG sys_cfg;
    NTSTATUS status_func;
    NTSTATUS status_clk;
    NTSTATUS status_cfg;
    UCHAR reg_mac_0610[6];
    UCHAR reg_mac_0050[6];
    NTSTATUS status_mac_0610;
    NTSTATUS status_mac_0050;
} RTW_PROBE_DATA, *PRTW_PROBE_DATA;

typedef struct _DEVICE_EXTENSION {
    PDEVICE_OBJECT FunctionalDeviceObject;
    PDEVICE_OBJECT LowerDeviceObject;
    PDEVICE_OBJECT PhysicalDeviceObject;
    BOOLEAN DeviceStarted;
    RTW_PROBE_DATA LastProbeData;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

static PDEVICE_EXTENSION g_pDevExt = NULL;

static NTSTATUS RTW_UsbReadReg(PDEVICE_OBJECT LowerDevice, USHORT addr, PVOID buffer, ULONG len) {
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    PIO_STACK_LOCATION nextStack;
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST urb;
    NTSTATUS status;

    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_VENDOR_DEVICE;
    urb.TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    urb.Request = RTW_USB_CMD_REQ;
    urb.Value = addr;
    urb.Index = 0x0000;
    urb.TransferBufferLength = len;
    urb.TransferBuffer = buffer;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_SUBMIT_URB,
        LowerDevice,
        NULL, 0, NULL, 0,
        TRUE, &event, &ioStatus
    );

    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;

    nextStack = IoGetNextIrpStackLocation(irp);
    nextStack->Parameters.Others.Argument1 = &urb;

    status = IoCallDriver(LowerDevice, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }

    return status;
}

static void ProbeRealtekDevice(PDEVICE_EXTENSION pDevExt) {
    pDevExt->LastProbeData.status_func = RTW_UsbReadReg(
        pDevExt->LowerDeviceObject, REG_SYS_FUNC_EN, &pDevExt->LastProbeData.func_en, sizeof(USHORT));

    pDevExt->LastProbeData.status_clk = RTW_UsbReadReg(
        pDevExt->LowerDeviceObject, REG_SYS_CLK_CTRL, &pDevExt->LastProbeData.clk_ctrl, sizeof(USHORT));

    pDevExt->LastProbeData.status_cfg = RTW_UsbReadReg(
        pDevExt->LowerDeviceObject, REG_SYS_CFG1, &pDevExt->LastProbeData.sys_cfg, sizeof(ULONG));

    pDevExt->LastProbeData.status_mac_0610 = RTW_UsbReadReg(
        pDevExt->LowerDeviceObject, REG_MACID_0610, pDevExt->LastProbeData.reg_mac_0610, 6);

    pDevExt->LastProbeData.status_mac_0050 = RTW_UsbReadReg(
        pDevExt->LowerDeviceObject, REG_MACID_0050, pDevExt->LastProbeData.reg_mac_0050, 6);

    DbgPrint("RTWPROBE: Hardware probed!\n");
}

static NTSTATUS NTAPI StartCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    PKEVENT pEvent = (PKEVENT)Context;
    KeSetEvent(pEvent, 0, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS NTAPI RTWProbe_Pnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PDEVICE_EXTENSION pDevExt = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    switch (irpStack->MinorFunction) {
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
                ProbeRealtekDevice(pDevExt);
            }

            Irp->IoStatus.Status = status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }

        case IRP_MN_REMOVE_DEVICE: {
            UNICODE_STRING symLink;
            RtlInitUnicodeString(&symLink, L"\\DosDevices\\RTWPROBE");
            IoDeleteSymbolicLink(&symLink);
            g_pDevExt = NULL;

            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            status = IoCallDriver(pDevExt->LowerDeviceObject, Irp);
            IoDetachDevice(pDevExt->LowerDeviceObject);
            /* Do NOT call IoDeleteDevice: avoids BugCheck 0x36 completely */
            return status;
        }

        default:
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(pDevExt->LowerDeviceObject, Irp);
    }
}

NTSTATUS NTAPI RTWProbe_Power(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PDEVICE_EXTENSION pDevExt = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(pDevExt->LowerDeviceObject, Irp);
}

NTSTATUS NTAPI RTWProbe_DeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PDEVICE_EXTENSION pDevExt = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG outLen = irpStack->Parameters.DeviceIoControl.OutputBufferLength;

    if (irpStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_RTW_PROBE) {
        if (outLen >= sizeof(RTW_PROBE_DATA)) {
            PRTW_PROBE_DATA pOut = (PRTW_PROBE_DATA)Irp->AssociatedIrp.SystemBuffer;
            ProbeRealtekDevice(pDevExt);
            *pOut = pDevExt->LastProbeData;
            Irp->IoStatus.Information = sizeof(RTW_PROBE_DATA);
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS NTAPI RTWProbe_CreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RTWProbe_AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
    PDEVICE_OBJECT fdo;
    PDEVICE_EXTENSION pDevExt;
    UNICODE_STRING ntDevName;
    UNICODE_STRING symLinkName;
    NTSTATUS status;

    RtlInitUnicodeString(&ntDevName, L"\\Device\\RTWPROBE");
    RtlInitUnicodeString(&symLinkName, L"\\DosDevices\\RTWPROBE");

    status = IoCreateDevice(
        DriverObject,
        sizeof(DEVICE_EXTENSION),
        &ntDevName,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &fdo
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    IoCreateSymbolicLink(&symLinkName, &ntDevName);

    pDevExt = (PDEVICE_EXTENSION)fdo->DeviceExtension;
    RtlZeroMemory(pDevExt, sizeof(DEVICE_EXTENSION));
    pDevExt->FunctionalDeviceObject = fdo;
    pDevExt->PhysicalDeviceObject = PhysicalDeviceObject;
    pDevExt->LowerDeviceObject = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);

    if (!pDevExt->LowerDeviceObject) {
        IoDeleteSymbolicLink(&symLinkName);
        IoDeleteDevice(fdo);
        return STATUS_DEVICE_REMOVED;
    }

    fdo->Flags |= DO_BUFFERED_IO;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    g_pDevExt = pDevExt;
    return STATUS_SUCCESS;
}

VOID NTAPI RTWProbe_Unload(PDRIVER_OBJECT DriverObject) {
    DbgPrint("RTWPROBE: Driver Unloaded\n");
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    DriverObject->DriverExtension->AddDevice = RTWProbe_AddDevice;
    DriverObject->MajorFunction[IRP_MJ_PNP] = RTWProbe_Pnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = RTWProbe_Power;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = RTWProbe_CreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = RTWProbe_CreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RTWProbe_DeviceControl;
    DriverObject->DriverUnload = RTWProbe_Unload;

    DbgPrint("RTWPROBE: DriverEntry Initialized\n");
    return STATUS_SUCCESS;
}
