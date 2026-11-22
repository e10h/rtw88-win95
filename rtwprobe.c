#include <ddk/wdm.h>
#include <usb.h>
#include <usbioctl.h>
#include <ddk/usbdlib.h>

#define RTW_USB_CMD_REQ     0x05
#define REG_SYS_FUNC_EN     0x0002
#define REG_SYS_CLK_CTRL    0x0008
#define REG_SYS_CFG1        0x00F0

#define IOCTL_RTW_PROBE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _RTW_PROBE_DATA {
    USHORT func_en;
    USHORT clk_ctrl;
    ULONG sys_cfg;
    NTSTATUS status_func;
    NTSTATUS status_clk;
    NTSTATUS status_cfg;
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

    DbgPrint("RTWPROBE: Hardware probed!\n");
}

static NTSTATUS NTAPI StartCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    PKEVENT pEvent = (PKEVENT)Context;
    KeSetEvent(pEvent, 0, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS NTAPI RtwPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
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
                DbgPrint("RTWPROBE: Start Device successful\n");
            }

            Irp->IoStatus.Status = status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }

        case IRP_MN_REMOVE_DEVICE: {
            IoSkipCurrentIrpStackLocation(Irp);
            status = IoCallDriver(pDevExt->LowerDeviceObject, Irp);
            IoDetachDevice(pDevExt->LowerDeviceObject);
            IoDeleteDevice(DeviceObject);
            return status;
        }

        default:
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(pDevExt->LowerDeviceObject, Irp);
    }
}

NTSTATUS NTAPI RtwCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtwDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_EXTENSION pDevExt = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG info = 0;

    if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_RTW_PROBE) {
        if (stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(RTW_PROBE_DATA)) {
            PRTW_PROBE_DATA out = (PRTW_PROBE_DATA)Irp->AssociatedIrp.SystemBuffer;
            ProbeRealtekDevice(pDevExt);
            *out = pDevExt->LastProbeData;
            info = sizeof(RTW_PROBE_DATA);
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS NTAPI RtwAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
    NTSTATUS status;
    PDEVICE_OBJECT fdo;
    PDEVICE_EXTENSION pDevExt;
    UNICODE_STRING devName, symName;

    RtlInitUnicodeString(&devName, L"\\Device\\RTWPROBE");
    RtlInitUnicodeString(&symName, L"\\DosDevices\\RTWPROBE");

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
    pDevExt->FunctionalDeviceObject = fdo;
    pDevExt->PhysicalDeviceObject = PhysicalDeviceObject;
    pDevExt->DeviceStarted = FALSE;
    g_pDevExt = pDevExt;

    pDevExt->LowerDeviceObject = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    if (!pDevExt->LowerDeviceObject) {
        IoDeleteDevice(fdo);
        return STATUS_DEVICE_REMOVED;
    }

    status = IoCreateSymbolicLink(&symName, &devName);
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    DbgPrint("RTWPROBE: AddDevice successful\n");
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtwPower(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PDEVICE_EXTENSION pDevExt = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(pDevExt->LowerDeviceObject, Irp);
}

void NTAPI RtwUnload(PDRIVER_OBJECT DriverObject) {
    UNICODE_STRING symName;
    RtlInitUnicodeString(&symName, L"\\DosDevices\\RTWPROBE");
    IoDeleteSymbolicLink(&symName);
    DbgPrint("RTWPROBE: Driver Unloaded\n");
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    DriverObject->DriverExtension->AddDevice = RtwAddDevice;
    DriverObject->DriverUnload = RtwUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = RtwCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = RtwCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RtwDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = RtwPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = RtwPower;

    DbgPrint("RTWPROBE: DriverEntry\n");
    return STATUS_SUCCESS;
}
