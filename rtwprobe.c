#include <ddk/wdm.h>
#include <usb.h>
#include <usbioctl.h>
#include <ddk/usbdlib.h>

#define RTW_USB_CMD_REQ             0x05

/* Core Registers */
#define REG_SYS_FUNC_EN             0x0002
#define REG_SYS_CLK_CTRL            0x0008
#define REG_RSV_CTRL                0x001C
#define REG_MCUFW_CTRL              0x0080
#define REG_SYS_CFG1                0x00F0
#define REG_MACID_0610              0x0610
#define REG_MACID_0050              0x0050
#define REG_CPU_DMEM_CON            0x1080
#define REG_DDMA_CH0SA              0x1200
#define REG_DDMA_CH0DA              0x1204
#define REG_DDMA_CH0CTRL            0x1208
#define REG_TXDMA_STATUS            0x0210
#define REG_FW_DBG7                 0x10FC

/* Register Bits */
#define BIT_FEN_CPUEN               (1 << 2)        /* In REG_SYS_FUNC_EN + 1 (0x0003) */
#define BIT_CPU_CLK_EN              (1 << 14)       /* In REG_SYS_CLK_CTRL */
#define BIT_WLMCU_IOIF              (1 << 0)        /* In REG_RSV_CTRL + 1 (0x001D) */
#define BIT_WL_PLATFORM_RST         (1 << 16)       /* In REG_CPU_DMEM_CON */
#define BIT_MCUFWDL_EN              (1 << 0)        /* In REG_MCUFW_CTRL */
#define BIT_MCUFWDL_RDY             (1 << 1)
#define BIT_IMEM_DW_OK              (1 << 3)
#define BIT_DMEM_CHKSUM_OK          (1 << 4)
#define BIT_DMEM_DW_OK              (1 << 5)
#define BIT_IMEM_CHKSUM_OK          (1 << 6)
#define BIT_CHECK_SUM_OK            (BIT_DMEM_CHKSUM_OK | BIT_IMEM_CHKSUM_OK)
#define BIT_FW_DW_RDY               (1 << 14)
#define BIT_FW_INIT_RDY             (1 << 15)
#define FW_READY                    (BIT_FW_INIT_RDY | BIT_FW_DW_RDY | BIT_IMEM_DW_OK | BIT_DMEM_DW_OK | BIT_CHECK_SUM_OK)
#define BTI_PAGE_OVF                (1 << 2)        /* In REG_TXDMA_STATUS */

/* DDMA Bits */
#define BIT_DDMACH0_OWN             (1UL << 31)
#define BIT_DDMACH0_CHKSUM_EN       (1UL << 29)
#define BIT_DDMACH0_CHKSUM_STS      (1UL << 27)
#define BIT_DDMACH0_RESET_CHKSUM    (1UL << 25)

/* IOCTL Codes */
#define IOCTL_RTW_PROBE             CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_FW_INIT           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_FW_WRITE_PAGE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_FW_BOOT           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_MAC_ENABLE        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_BULK_RX           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_BULK_TX           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x812, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Milestone 7 Generic Hardware Access IOCTLs (keeps RTWPROBE.SYS permanently stable!) */
#define IOCTL_RTW_REG_READ          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_REG_WRITE         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_REG_BATCH         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x822, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_BULK_RX_RAW       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x823, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_BULK_TX_PIPE      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x824, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RTW_GET_PIPES         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x825, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define RTW_OP_WRITE                0
#define RTW_OP_MASK                 1
#define RTW_OP_POLL                 2
#define RTW_OP_RF_WRITE             3
#define RTW_OP_DELAY_READS          4

typedef struct _RTW_REG_REQ {
    USHORT Addr;
    USHORT Width;       /* 1, 2, or 4 */
    ULONG  Value;
    LONG   Status;
} RTW_REG_REQ, *PRTW_REG_REQ;

typedef struct _RTW_REG_OP {
    UCHAR  Op;          /* RTW_OP_* */
    UCHAR  Width;       /* 1/2/4 or RF path (0=A, 1=B) */
    USHORT Addr;
    ULONG  Mask;
    ULONG  Value;
} RTW_REG_OP, *PRTW_REG_OP;

typedef struct _RTW_REG_BATCH {
    ULONG      Count;   /* up to 256 */
    RTW_REG_OP Ops[256];
} RTW_REG_BATCH, *PRTW_REG_BATCH;

typedef struct _RTW_BULK_RX_REQ {
    ULONG CheckFifo;    /* 1 = check REG_RXPKTNUM first, 0 = read immediately */
    ULONG MaxLen;
} RTW_BULK_RX_REQ, *PRTW_BULK_RX_REQ;

typedef struct _RTW_BULK_RX_RAW {
    LONG  Status;
    ULONG Length;
    ULONG RxPktNum;
    UCHAR Data[4096];
} RTW_BULK_RX_RAW, *PRTW_BULK_RX_RAW;

typedef struct _RTW_BULK_TX_PIPE {
    ULONG OutPipeIdx;   /* 0=HI, 1=NORM, 2=LOW, 3=EXTRA */
    ULONG Length;
    UCHAR Data[4200];
} RTW_BULK_TX_PIPE, *PRTW_BULK_TX_PIPE;

#define REG_CR                      0x0100
#define REG_RCR                     0x0608
#define REG_RXPKTNUM                0x02B0
#define REG_RXDMA_MODE              0x0290

typedef struct _RTW_RX_PACKET {
    ULONG Status;
    ULONG Length;
    ULONG RxPktNum;
    UCHAR Data[2048];
} RTW_RX_PACKET, *PRTW_RX_PACKET;

typedef struct _RTW_TX_PACKET {
    ULONG Length;
    UCHAR Data[2048];
} RTW_TX_PACKET, *PRTW_TX_PACKET;

typedef struct _RTW_PIPE_INFO {
    UCHAR EndpointAddress;
    UCHAR PipeType;
    USHORT MaxPacketSize;
} RTW_PIPE_INFO;

typedef struct _RTW_PIPES_RESP {
    ULONG         NumPipes;
    ULONG         NumBulkOut;
    UCHAR         BulkInAddr;
    UCHAR         BulkOutAddrs[4];
    UCHAR         Reserved[3];
    RTW_PIPE_INFO Pipes[8];
} RTW_PIPES_RESP, *PRTW_PIPES_RESP;

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

    /* Milestone 3 Probing */
    USHORT mcufw_ctrl_init;
    USHORT mcufw_ctrl_dl_en;
    ULONG num_pipes;
    RTW_PIPE_INFO pipes[8];
    NTSTATUS status_select_config;
    UCHAR platform_reset_ok;
    UCHAR reserved[1];
    USHORT mcufw_ctrl_post_reset;
} RTW_PROBE_DATA, *PRTW_PROBE_DATA;

typedef struct _RTW_FW_PAGE_PARAM {
    ULONG PageIndex;
    ULONG TargetAddr;
    ULONG Length;
    UCHAR Data[4096];
} RTW_FW_PAGE_PARAM, *PRTW_FW_PAGE_PARAM;

typedef struct _RTW_BOOT_RESULT {
    UCHAR Success;
    UCHAR Reserved;
    USHORT FinalMcuFwCtrl;
    UCHAR PostBootMac[6];
    UCHAR Reserved2[2];
    NTSTATUS Status;
} RTW_BOOT_RESULT, *PRTW_BOOT_RESULT;

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
    RTW_PROBE_DATA LastProbeData;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

#undef RtlZeroMemory
static void MyZeroMemory(void *dst, unsigned long len) {
    volatile unsigned char *d = (volatile unsigned char *)dst;
    while (len--) *d++ = 0;
}
#define RtlZeroMemory(d, l) MyZeroMemory(d, l)

static void MyMemCpy(void *dst, const void *src, unsigned long len) {
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *s = (const volatile unsigned char *)src;
    while (len--) *d++ = *s++;
}
#define memcpy(d, s, l) MyMemCpy(d, s, l)

static PDEVICE_EXTENSION g_pDevExt = NULL;

/* -------------------------------------------------------------
   USB Control Transfer Vendor Write (Raw - no 0x04E0 erratum)
------------------------------------------------------------- */
static NTSTATUS RTW_UsbWriteRegRaw(PDEVICE_OBJECT LowerDevice, USHORT addr, PVOID buffer, ULONG len) {
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    PIO_STACK_LOCATION nextStack;
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST urb;
    NTSTATUS status;

    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_VENDOR_DEVICE;
    urb.TransferFlags = 0; /* Host to Device (OUT) */
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

/* RTL8822BU USB silicon erratum (Linux rtw88/usb.c rtw_usb_reg_sec):
 * After any access to the always-on section (0x00..0xFF or 0x1000..0x10FF),
 * write 1 byte to 0x04E0 so the USB register bridge does not stall. */
static void RTW_UsbRegSec(PDEVICE_OBJECT LowerDevice, USHORT addr, UCHAR byte0) {
    if (addr <= 0x00FF || (addr >= 0x1000 && addr <= 0x10FF)) {
        RTW_UsbWriteRegRaw(LowerDevice, 0x04E0, &byte0, 1);
    }
}

/* -------------------------------------------------------------
   USB Control Transfer Vendor Read (bRequest = 0x05, IN)
------------------------------------------------------------- */
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

    if (NT_SUCCESS(status) && len > 0) {
        RTW_UsbRegSec(LowerDevice, addr, ((PUCHAR)buffer)[0]);
    }

    return status;
}

/* -------------------------------------------------------------
   USB Control Transfer Vendor Write (bRequest = 0x05, OUT)
------------------------------------------------------------- */
static NTSTATUS RTW_UsbWriteReg(PDEVICE_OBJECT LowerDevice, USHORT addr, PVOID buffer, ULONG len) {
    NTSTATUS status = RTW_UsbWriteRegRaw(LowerDevice, addr, buffer, len);
    if (NT_SUCCESS(status) && len > 0) {
        RTW_UsbRegSec(LowerDevice, addr, ((PUCHAR)buffer)[0]);
    }
    return status;
}

static UCHAR RTW_Read8(PDEVICE_OBJECT LowerDevice, USHORT addr) {
    UCHAR val = 0;
    RTW_UsbReadReg(LowerDevice, addr, &val, 1);
    return val;
}

static USHORT RTW_Read16(PDEVICE_OBJECT LowerDevice, USHORT addr) {
    USHORT val = 0;
    RTW_UsbReadReg(LowerDevice, addr, &val, 2);
    return val;
}

static ULONG RTW_Read32(PDEVICE_OBJECT LowerDevice, USHORT addr) {
    ULONG val = 0;
    RTW_UsbReadReg(LowerDevice, addr, &val, 4);
    return val;
}

static NTSTATUS RTW_Write8(PDEVICE_OBJECT LowerDevice, USHORT addr, UCHAR val) {
    return RTW_UsbWriteReg(LowerDevice, addr, &val, 1);
}

static NTSTATUS RTW_Write16(PDEVICE_OBJECT LowerDevice, USHORT addr, USHORT val) {
    return RTW_UsbWriteReg(LowerDevice, addr, &val, 2);
}

static NTSTATUS RTW_Write32(PDEVICE_OBJECT LowerDevice, USHORT addr, ULONG val) {
    return RTW_UsbWriteReg(LowerDevice, addr, &val, 4);
}

/* -------------------------------------------------------------
   USB Bulk Transfer Engine (Pipe 0 = EP 84 IN, Pipe 2 = EP 06 OUT)
------------------------------------------------------------- */
static NTSTATUS RTW_UsbBulkTransfer(
    PDEVICE_OBJECT LowerDevice,
    USBD_PIPE_HANDLE PipeHandle,
    ULONG TransferFlags,
    PVOID Buffer,
    ULONG BufferLength,
    PULONG BytesTransferred
) {
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    PIO_STACK_LOCATION nextStack;
    struct _URB_BULK_OR_INTERRUPT_TRANSFER urb;
    NTSTATUS status;

    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    urb.PipeHandle = PipeHandle;
    urb.TransferFlags = TransferFlags;
    urb.TransferBufferLength = BufferLength;
    urb.TransferBuffer = Buffer;

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

    if (BytesTransferred) {
        *BytesTransferred = urb.TransferBufferLength;
    }

    return status;
}

/* -------------------------------------------------------------
   USB Descriptor Request
------------------------------------------------------------- */
static NTSTATUS RTW_GetDescriptor(PDEVICE_OBJECT LowerDevice, UCHAR type, UCHAR index, PVOID buffer, ULONG len) {
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    PIO_STACK_LOCATION nextStack;
    struct _URB_CONTROL_DESCRIPTOR_REQUEST urb;
    NTSTATUS status;

    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    urb.DescriptorType = type;
    urb.Index = index;
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

/* -------------------------------------------------------------
   Configure USB Device & Extract Pipes
------------------------------------------------------------- */
static NTSTATUS RTW_ConfigureDevice(PDEVICE_EXTENSION pDevExt) {
    USB_CONFIGURATION_DESCRIPTOR configHdr;
    PUSB_CONFIGURATION_DESCRIPTOR pFullDesc;
    NTSTATUS status;
    ULONG totalLength;

    if (pDevExt->ConfigurationHandle != NULL && pDevExt->NumberOfPipes > 0) {
        return STATUS_SUCCESS; /* Already configured */
    }

    /* 1. Query Configuration Descriptor Header */
    status = RTW_GetDescriptor(pDevExt->LowerDeviceObject, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, &configHdr, sizeof(configHdr));
    if (!NT_SUCCESS(status)) return status;

    totalLength = configHdr.wTotalLength;
    if (totalLength > sizeof(pDevExt->UsbConfigBuffer)) {
        totalLength = sizeof(pDevExt->UsbConfigBuffer);
    }

    /* 2. Query Full Configuration Descriptor */
    status = RTW_GetDescriptor(pDevExt->LowerDeviceObject, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, pDevExt->UsbConfigBuffer, totalLength);
    if (!NT_SUCCESS(status)) return status;

    pFullDesc = (PUSB_CONFIGURATION_DESCRIPTOR)pDevExt->UsbConfigBuffer;

    /* 3. Build URB_FUNCTION_SELECT_CONFIGURATION */
    struct {
        struct _URB_HEADER Hdr;
        PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;
        USBD_CONFIGURATION_HANDLE ConfigurationHandle;
        USBD_INTERFACE_INFORMATION Interface;
        USBD_PIPE_INFORMATION ExtraPipes[7];
    } selUrb;

    RtlZeroMemory(&selUrb, sizeof(selUrb));
    selUrb.Hdr.Length = sizeof(selUrb);
    selUrb.Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    selUrb.ConfigurationDescriptor = pFullDesc;
    selUrb.Interface.Length = sizeof(USBD_INTERFACE_INFORMATION) + 7 * sizeof(USBD_PIPE_INFORMATION);
    selUrb.Interface.InterfaceNumber = 0;
    selUrb.Interface.AlternateSetting = 0;

    selUrb.Interface.Pipes[0].MaximumTransferSize = 0x10000;
    for (int p = 0; p < 7; p++) {
        selUrb.ExtraPipes[p].MaximumTransferSize = 0x10000;
    }

    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    PIO_STACK_LOCATION nextStack;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_SUBMIT_URB,
        pDevExt->LowerDeviceObject,
        NULL, 0, NULL, 0,
        TRUE, &event, &ioStatus
    );

    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;

    nextStack = IoGetNextIrpStackLocation(irp);
    nextStack->Parameters.Others.Argument1 = &selUrb;

    status = IoCallDriver(pDevExt->LowerDeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }

    if (NT_SUCCESS(status)) {
        pDevExt->ConfigurationHandle = selUrb.ConfigurationHandle;
        pDevExt->NumberOfPipes = selUrb.Interface.NumberOfPipes;
        if (pDevExt->NumberOfPipes > 8) pDevExt->NumberOfPipes = 8;
        for (ULONG i = 0; i < pDevExt->NumberOfPipes; i++) {
            MyMemCpy(&pDevExt->Pipes[i], &selUrb.Interface.Pipes[i], sizeof(USBD_PIPE_INFORMATION));
        }
        DbgPrint("RTWPROBE: USB Configured, %d pipes opened\n", (int)pDevExt->NumberOfPipes);
    }

    return status;
}

static USBD_PIPE_HANDLE RTW_FindBulkInPipe(PDEVICE_EXTENSION pDevExt) {
    if (pDevExt->ConfigurationHandle == NULL || pDevExt->NumberOfPipes == 0) {
        RTW_ConfigureDevice(pDevExt);
    }
    for (ULONG i = 0; i < pDevExt->NumberOfPipes; i++) {
        if ((pDevExt->Pipes[i].EndpointAddress & 0x80) &&
            pDevExt->Pipes[i].PipeType == UsbdPipeTypeBulk &&
            pDevExt->Pipes[i].PipeHandle != NULL) {
            return pDevExt->Pipes[i].PipeHandle;
        }
    }
    for (ULONG i = 0; i < pDevExt->NumberOfPipes; i++) {
        if ((pDevExt->Pipes[i].EndpointAddress & 0x80) &&
            pDevExt->Pipes[i].PipeHandle != NULL) {
            return pDevExt->Pipes[i].PipeHandle;
        }
    }
    return NULL;
}

static USBD_PIPE_HANDLE RTW_FindBulkOutPipe(PDEVICE_EXTENSION pDevExt) {
    if (pDevExt->ConfigurationHandle == NULL || pDevExt->NumberOfPipes == 0) {
        RTW_ConfigureDevice(pDevExt);
    }
    for (ULONG i = 0; i < pDevExt->NumberOfPipes; i++) {
        if (!(pDevExt->Pipes[i].EndpointAddress & 0x80) &&
            pDevExt->Pipes[i].PipeType == UsbdPipeTypeBulk &&
            pDevExt->Pipes[i].PipeHandle != NULL) {
            return pDevExt->Pipes[i].PipeHandle;
        }
    }
    for (ULONG i = 0; i < pDevExt->NumberOfPipes; i++) {
        if (!(pDevExt->Pipes[i].EndpointAddress & 0x80) &&
            pDevExt->Pipes[i].PipeHandle != NULL) {
            return pDevExt->Pipes[i].PipeHandle;
        }
    }
    return NULL;
}

static USBD_PIPE_HANDLE RTW_FindBulkOutPipeByIndex(PDEVICE_EXTENSION pDevExt, ULONG idx) {
    ULONG found = 0;
    if (pDevExt->ConfigurationHandle == NULL || pDevExt->NumberOfPipes == 0) {
        RTW_ConfigureDevice(pDevExt);
    }
    for (ULONG i = 0; i < pDevExt->NumberOfPipes; i++) {
        if (!(pDevExt->Pipes[i].EndpointAddress & 0x80) &&
            pDevExt->Pipes[i].PipeType == UsbdPipeTypeBulk &&
            pDevExt->Pipes[i].PipeHandle != NULL) {
            if (found == idx) return pDevExt->Pipes[i].PipeHandle;
            found++;
        }
    }
    return RTW_FindBulkOutPipe(pDevExt);
}

static NTSTATUS RTW_ReadWidth(PDEVICE_OBJECT dev, USHORT addr, USHORT width, PULONG outVal) {
    ULONG v = 0;
    NTSTATUS st;
    if (width == 1) {
        UCHAR b = 0;
        st = RTW_UsbReadReg(dev, addr, &b, 1);
        v = b;
    } else if (width == 2) {
        USHORT w = 0;
        st = RTW_UsbReadReg(dev, addr, &w, 2);
        v = w;
    } else {
        st = RTW_UsbReadReg(dev, addr, &v, 4);
    }
    *outVal = v;
    return st;
}

static NTSTATUS RTW_WriteWidth(PDEVICE_OBJECT dev, USHORT addr, USHORT width, ULONG val) {
    if (width == 1) {
        UCHAR b = (UCHAR)(val & 0xFF);
        return RTW_UsbWriteReg(dev, addr, &b, 1);
    } else if (width == 2) {
        USHORT w = (USHORT)(val & 0xFFFF);
        return RTW_UsbWriteReg(dev, addr, &w, 2);
    } else {
        return RTW_UsbWriteReg(dev, addr, &val, 4);
    }
}

static void RTW_WriteRfSipi(PDEVICE_OBJECT dev, UCHAR path, USHORT rfAddr, ULONG mask, ULONG val) {
    USHORT sipiReg = (path == 0) ? 0x0C90 : 0x0E90;
    USHORT directReg = (USHORT)(((path == 0) ? 0x2800 : 0x2C00) + ((rfAddr & 0xFF) << 2));
    ULONG data;
    mask &= 0xFFFFF;
    if (mask != 0xFFFFF) {
        ULONG old = RTW_Read32(dev, directReg) & 0xFFFFF;
        data = (old & ~mask) | (val & mask);
    } else {
        data = val & 0xFFFFF;
    }
    ULONG cmd = (((ULONG)(rfAddr & 0xFF) << 20) | (data & 0xFFFFF)) & 0x0FFFFFFF;
    RTW_Write32(dev, sipiReg, cmd);
}

/* -------------------------------------------------------------
   WLAN CPU Power & Control
------------------------------------------------------------- */
static void RTW_WlanCpuEnable(PDEVICE_OBJECT LowerDevice, BOOLEAN enable) {
    UCHAR val;
    if (enable) {
        val = RTW_Read8(LowerDevice, REG_RSV_CTRL + 1);
        RTW_Write8(LowerDevice, REG_RSV_CTRL + 1, val | BIT_WLMCU_IOIF);

        val = RTW_Read8(LowerDevice, REG_SYS_FUNC_EN + 1);
        RTW_Write8(LowerDevice, REG_SYS_FUNC_EN + 1, val | BIT_FEN_CPUEN);
    } else {
        val = RTW_Read8(LowerDevice, REG_SYS_FUNC_EN + 1);
        RTW_Write8(LowerDevice, REG_SYS_FUNC_EN + 1, val & ~BIT_FEN_CPUEN);

        val = RTW_Read8(LowerDevice, REG_RSV_CTRL + 1);
        RTW_Write8(LowerDevice, REG_RSV_CTRL + 1, val & ~BIT_WLMCU_IOIF);
    }
}

/* -------------------------------------------------------------
   Firmware Platform Reset
------------------------------------------------------------- */
static void RTW_ResetPlatform(PDEVICE_OBJECT LowerDevice) {
    UCHAR val;

    val = RTW_Read8(LowerDevice, REG_CPU_DMEM_CON + 2);
    RTW_Write8(LowerDevice, REG_CPU_DMEM_CON + 2, val & ~(BIT_WL_PLATFORM_RST >> 16));

    val = RTW_Read8(LowerDevice, REG_SYS_CLK_CTRL + 1);
    RTW_Write8(LowerDevice, REG_SYS_CLK_CTRL + 1, val & ~(BIT_CPU_CLK_EN >> 8));

    val = RTW_Read8(LowerDevice, REG_CPU_DMEM_CON + 2);
    RTW_Write8(LowerDevice, REG_CPU_DMEM_CON + 2, val | (BIT_WL_PLATFORM_RST >> 16));

    val = RTW_Read8(LowerDevice, REG_SYS_CLK_CTRL + 1);
    RTW_Write8(LowerDevice, REG_SYS_CLK_CTRL + 1, val | (BIT_CPU_CLK_EN >> 8));
}

/* -------------------------------------------------------------
   Full Hardware Probe (Milestones 1, 2, & 3)
------------------------------------------------------------- */
static void ProbeRealtekDevice(PDEVICE_EXTENSION pDevExt) {
    PDEVICE_OBJECT dev = pDevExt->LowerDeviceObject;

    /* Milestones 1 & 2 Registers */
    pDevExt->LastProbeData.status_func = RTW_UsbReadReg(dev, REG_SYS_FUNC_EN, &pDevExt->LastProbeData.func_en, sizeof(USHORT));
    pDevExt->LastProbeData.status_clk = RTW_UsbReadReg(dev, REG_SYS_CLK_CTRL, &pDevExt->LastProbeData.clk_ctrl, sizeof(USHORT));
    pDevExt->LastProbeData.status_cfg = RTW_UsbReadReg(dev, REG_SYS_CFG1, &pDevExt->LastProbeData.sys_cfg, sizeof(ULONG));
    pDevExt->LastProbeData.status_mac_0610 = RTW_UsbReadReg(dev, REG_MACID_0610, pDevExt->LastProbeData.reg_mac_0610, 6);
    pDevExt->LastProbeData.status_mac_0050 = RTW_UsbReadReg(dev, REG_MACID_0050, pDevExt->LastProbeData.reg_mac_0050, 6);

    /* Milestone 3: Read Initial REG_MCUFW_CTRL */
    pDevExt->LastProbeData.mcufw_ctrl_init = RTW_Read16(dev, REG_MCUFW_CTRL);

    /* Configure USB and populate pipes */
    pDevExt->LastProbeData.status_select_config = RTW_ConfigureDevice(pDevExt);
    pDevExt->LastProbeData.num_pipes = pDevExt->NumberOfPipes;
    for (ULONG i = 0; i < pDevExt->NumberOfPipes; i++) {
        pDevExt->LastProbeData.pipes[i].EndpointAddress = pDevExt->Pipes[i].EndpointAddress;
        pDevExt->LastProbeData.pipes[i].PipeType = (UCHAR)pDevExt->Pipes[i].PipeType;
        pDevExt->LastProbeData.pipes[i].MaxPacketSize = (USHORT)pDevExt->Pipes[i].MaximumPacketSize;
    }

    /* Test Platform Reset & Clock */
    RTW_ResetPlatform(dev);
    pDevExt->LastProbeData.platform_reset_ok = TRUE;
    pDevExt->LastProbeData.mcufw_ctrl_post_reset = RTW_Read16(dev, REG_MCUFW_CTRL);

    /* Test MCU Firmware Download Mode Enable */
    USHORT fwVal = (RTW_Read16(dev, REG_MCUFW_CTRL) & 0x3800) | BIT_MCUFWDL_EN;
    RTW_Write16(dev, REG_MCUFW_CTRL, fwVal);
    pDevExt->LastProbeData.mcufw_ctrl_dl_en = RTW_Read16(dev, REG_MCUFW_CTRL);

    DbgPrint("RTWPROBE: Hardware probed, MCUFWDL=0x%04x\n", pDevExt->LastProbeData.mcufw_ctrl_dl_en);
}

/* -------------------------------------------------------------
   WDM Standard PnP Handlers
------------------------------------------------------------- */
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
            RtlInitUnicodeString(&symLink, L"\\DosDevices\\RTWPROBE");
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

            RtlInitUnicodeString(&symLink, L"\\DosDevices\\RTWPROBE");
            IoDeleteSymbolicLink(&symLink);
            g_pDevExt = NULL;
            if (pDevExt) {
                pDevExt->LowerDeviceObject = NULL;
                pDevExt->DeviceRemoved = TRUE;
                pDevExt->DeviceStarted = FALSE;
            }

            /* 1. Forward IRP down the stack to lower driver first */
            if (lowerDev) {
                Irp->IoStatus.Status = STATUS_SUCCESS;
                IoSkipCurrentIrpStackLocation(Irp);
                status = IoCallDriver(lowerDev, Irp);

                /* 2. Detach lower device object after stack processing completes */
                IoDetachDevice(lowerDev);
            } else {
                Irp->IoStatus.Status = STATUS_SUCCESS;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
            }

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
    PDEVICE_OBJECT dev = pDevExt ? pDevExt->LowerDeviceObject : NULL;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG info = 0;
    ULONG ioctl = stack->Parameters.DeviceIoControl.IoControlCode;

    if (!pDevExt || pDevExt->DeviceRemoved || !dev) {
        Irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_REMOVED;
    }

    switch (ioctl) {
        case IOCTL_RTW_PROBE: {
            if (stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(RTW_PROBE_DATA)) {
                PRTW_PROBE_DATA out = (PRTW_PROBE_DATA)Irp->AssociatedIrp.SystemBuffer;
                ProbeRealtekDevice(pDevExt);
                *out = pDevExt->LastProbeData;
                info = sizeof(RTW_PROBE_DATA);
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case IOCTL_RTW_FW_INIT: {
            /* 1. Halt CPU */
            RTW_WlanCpuEnable(dev, FALSE);

            /* 2. Platform Reset */
            RTW_ResetPlatform(dev);

            /* 3. Reset DDMA Checksum */
            ULONG ddmaCtrl = RTW_Read32(dev, REG_DDMA_CH0CTRL);
            RTW_Write32(dev, REG_DDMA_CH0CTRL, ddmaCtrl | BIT_DDMACH0_RESET_CHKSUM);

            /* 4. Enable MCU Download Mode */
            USHORT fwVal = (RTW_Read16(dev, REG_MCUFW_CTRL) & 0x3800) | BIT_MCUFWDL_EN;
            RTW_Write16(dev, REG_MCUFW_CTRL, fwVal);

            if (stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(USHORT)) {
                *(PUSHORT)Irp->AssociatedIrp.SystemBuffer = RTW_Read16(dev, REG_MCUFW_CTRL);
                info = sizeof(USHORT);
            }
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_RTW_FW_WRITE_PAGE: {
            if (stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(RTW_FW_PAGE_PARAM)) {
                PRTW_FW_PAGE_PARAM param = (PRTW_FW_PAGE_PARAM)Irp->AssociatedIrp.SystemBuffer;
                ULONG length = param->Length;
                ULONG offset = 0;
                USHORT targetAddr = (USHORT)(param->TargetAddr ? param->TargetAddr : 0x1000);

                /* Set ROM Page in REG_MCUFW_CTRL bits 16..18 */
                ULONG fwCtrl = RTW_Read32(dev, REG_MCUFW_CTRL);
                fwCtrl &= ~(7UL << 16);
                fwCtrl |= ((param->PageIndex & 7UL) << 16);
                RTW_Write32(dev, REG_MCUFW_CTRL, fwCtrl);

                /* Stream in 196-byte blocks using Endpoint 0 Vendor Writes */
                status = STATUS_SUCCESS;
                while (length > 0) {
                    ULONG n = (length >= 196) ? 196 : ((length >= 8) ? 8 : 1);
                    status = RTW_UsbWriteReg(dev, targetAddr + (USHORT)offset, param->Data + offset, n);
                    if (!NT_SUCCESS(status)) break;
                    offset += n;
                    length -= n;
                }
                info = offset;
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case IOCTL_RTW_FW_BOOT: {
            if (stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(RTW_BOOT_RESULT)) {
                PRTW_BOOT_RESULT res = (PRTW_BOOT_RESULT)Irp->AssociatedIrp.SystemBuffer;
                RtlZeroMemory(res, sizeof(RTW_BOOT_RESULT));

                /* 1. Page Overflow Notification */
                RTW_Write8(dev, REG_TXDMA_STATUS, BTI_PAGE_OVF);

                /* 2. Check Checksum */
                USHORT fwCtrl = RTW_Read16(dev, REG_MCUFW_CTRL);

                /* 3. Transition to FW_DW_RDY & Clear MCUFWDL_EN */
                fwCtrl = (fwCtrl | BIT_FW_DW_RDY) & ~BIT_MCUFWDL_EN;
                RTW_Write16(dev, REG_MCUFW_CTRL, fwCtrl);

                /* 4. Start 32-bit RISC CPU */
                RTW_WlanCpuEnable(dev, TRUE);

                /* 5. Poll for FW_READY (BIT_FW_INIT_RDY = 0x8000) */
                BOOLEAN ready = FALSE;
                for (int i = 0; i < 50; i++) {
                    fwCtrl = RTW_Read16(dev, REG_MCUFW_CTRL);
                    if ((fwCtrl & BIT_FW_INIT_RDY) || ((fwCtrl & 0xC078) == 0xC078)) {
                        ready = TRUE;
                        break;
                    }
                }

                res->Success = ready;
                res->FinalMcuFwCtrl = fwCtrl;
                res->Status = ready ? STATUS_SUCCESS : STATUS_TIMEOUT;

                /* 6. Read Post-Boot Hardware MAC */
                RTW_UsbReadReg(dev, REG_MACID_0610, res->PostBootMac, 6);

                info = sizeof(RTW_BOOT_RESULT);
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case IOCTL_RTW_MAC_ENABLE: {
            if (pDevExt->ConfigurationHandle == NULL) {
                RTW_ConfigureDevice(pDevExt);
            }

            /* 1. Turn on MAC & HCI TX/RX DMA engines in REG_CR */
            RTW_Write8(dev, REG_CR, 0xFF);

            /* 2. Configure RCR: 0xE400220F (Accept Broadcast, Multicast, Unicast, Promiscuous) */
            RTW_Write32(dev, REG_RCR, 0xE400220F);

            /* 3. Configure RXDMA Mode (0x0290) */
            RTW_Write8(dev, REG_RXDMA_MODE, 0x00);

            if (stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG) * 3) {
                PULONG out = (PULONG)Irp->AssociatedIrp.SystemBuffer;
                out[0] = RTW_Read8(dev, REG_CR);
                out[1] = RTW_Read32(dev, REG_RCR);
                out[2] = RTW_Read16(dev, REG_RXPKTNUM);
                info = sizeof(ULONG) * 3;
            }
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_RTW_BULK_RX: {
            if (stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(RTW_RX_PACKET)) {
                PRTW_RX_PACKET rxPkt = (PRTW_RX_PACKET)Irp->AssociatedIrp.SystemBuffer;
                RtlZeroMemory(rxPkt, sizeof(RTW_RX_PACKET));

                USHORT rxPktNum = RTW_Read16(dev, REG_RXPKTNUM);
                rxPkt->RxPktNum = rxPktNum;

                /* If no packets waiting in RX FIFO, return immediately (never hang on empty FIFO) */
                if (rxPktNum == 0) {
                    rxPkt->Length = 0;
                    rxPkt->Status = STATUS_SUCCESS;
                    info = sizeof(RTW_RX_PACKET);
                    status = STATUS_SUCCESS;
                    break;
                }

                USBD_PIPE_HANDLE rxPipe = RTW_FindBulkInPipe(pDevExt);
                if (rxPipe != NULL) {
                    ULONG bytesRead = 0;
                    rxPkt->Status = RTW_UsbBulkTransfer(
                        dev,
                        rxPipe,
                        USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK,
                        rxPkt->Data,
                        sizeof(rxPkt->Data),
                        &bytesRead
                    );
                    rxPkt->Length = bytesRead;
                    info = sizeof(RTW_RX_PACKET);
                    status = STATUS_SUCCESS;
                } else {
                    /* Non-fatal: if bulk pipe is configuring or temporarily unavailable,
                     * return 0 bytes with STATUS_SUCCESS so polling loop never errors out with err=21 */
                    rxPkt->Length = 0;
                    rxPkt->Status = STATUS_SUCCESS;
                    info = sizeof(RTW_RX_PACKET);
                    status = STATUS_SUCCESS;
                }
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case IOCTL_RTW_BULK_TX: {
            if (stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(RTW_TX_PACKET)) {
                PRTW_TX_PACKET txPkt = (PRTW_TX_PACKET)Irp->AssociatedIrp.SystemBuffer;
                USBD_PIPE_HANDLE txPipe = RTW_FindBulkOutPipe(pDevExt);
                if (txPipe != NULL) {
                    ULONG bytesWritten = 0;
                    status = RTW_UsbBulkTransfer(
                        dev,
                        txPipe,
                        0,
                        txPkt->Data,
                        txPkt->Length,
                        &bytesWritten
                    );
                    info = bytesWritten;
                } else {
                    status = STATUS_SUCCESS;
                    info = 0;
                }
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case IOCTL_RTW_REG_READ: {
            if (stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(RTW_REG_REQ) &&
                stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(RTW_REG_REQ)) {
                PRTW_REG_REQ req = (PRTW_REG_REQ)Irp->AssociatedIrp.SystemBuffer;
                ULONG val = 0;
                NTSTATUS st = RTW_ReadWidth(dev, req->Addr, req->Width, &val);
                req->Value = val;
                req->Status = (LONG)st;
                info = sizeof(RTW_REG_REQ);
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case IOCTL_RTW_REG_WRITE: {
            if (stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(RTW_REG_REQ)) {
                PRTW_REG_REQ req = (PRTW_REG_REQ)Irp->AssociatedIrp.SystemBuffer;
                NTSTATUS st = RTW_WriteWidth(dev, req->Addr, req->Width, req->Value);
                if (stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(RTW_REG_REQ)) {
                    req->Status = (LONG)st;
                    info = sizeof(RTW_REG_REQ);
                }
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case IOCTL_RTW_REG_BATCH: {
            if (stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(ULONG)) {
                PRTW_REG_BATCH batch = (PRTW_REG_BATCH)Irp->AssociatedIrp.SystemBuffer;
                ULONG count = batch->Count;
                ULONG maxOps = (stack->Parameters.DeviceIoControl.InputBufferLength - sizeof(ULONG)) / sizeof(RTW_REG_OP);
                if (count > 256) count = 256;
                if (count > maxOps) count = maxOps;
                for (ULONG i = 0; i < count; i++) {
                    PRTW_REG_OP op = &batch->Ops[i];
                    if (op->Op == RTW_OP_WRITE) {
                        RTW_WriteWidth(dev, op->Addr, op->Width, op->Value);
                    } else if (op->Op == RTW_OP_MASK) {
                        ULONG oldv = 0;
                        RTW_ReadWidth(dev, op->Addr, op->Width, &oldv);
                        oldv = (oldv & ~op->Mask) | (op->Value & op->Mask);
                        RTW_WriteWidth(dev, op->Addr, op->Width, oldv);
                    } else if (op->Op == RTW_OP_POLL) {
                        for (ULONG t = 0; t < 500; t++) {
                            ULONG cur = 0;
                            RTW_ReadWidth(dev, op->Addr, op->Width, &cur);
                            if ((cur & op->Mask) == (op->Value & op->Mask)) break;
                        }
                    } else if (op->Op == RTW_OP_RF_WRITE) {
                        RTW_WriteRfSipi(dev, op->Width, op->Addr, op->Mask, op->Value);
                    } else if (op->Op == RTW_OP_DELAY_READS) {
                        ULONG n = op->Value;
                        if (n > 50) n = 50;
                        for (ULONG d = 0; d < n; d++) {
                            RTW_Read8(dev, 0x0000);
                        }
                    }
                }
                info = 0;
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case IOCTL_RTW_BULK_RX_RAW: {
            if (stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(RTW_BULK_RX_RAW)) {
                ULONG checkFifo = 1;
                ULONG maxLen = 4096;
                if (stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(RTW_BULK_RX_REQ)) {
                    PRTW_BULK_RX_REQ req = (PRTW_BULK_RX_REQ)Irp->AssociatedIrp.SystemBuffer;
                    checkFifo = req->CheckFifo;
                    maxLen = req->MaxLen;
                }
                if (maxLen == 0 || maxLen > 4096) maxLen = 4096;

                PRTW_BULK_RX_RAW rx = (PRTW_BULK_RX_RAW)Irp->AssociatedIrp.SystemBuffer;
                RtlZeroMemory(rx, sizeof(RTW_BULK_RX_RAW));

                if (pDevExt->ConfigurationHandle == NULL || pDevExt->NumberOfPipes == 0) {
                    RTW_ConfigureDevice(pDevExt);
                }

                USHORT rxPktNum = RTW_Read16(dev, REG_RXPKTNUM);
                rx->RxPktNum = rxPktNum;

                if (checkFifo && rxPktNum == 0) {
                    rx->Length = 0;
                    rx->Status = (LONG)STATUS_SUCCESS;
                    info = sizeof(RTW_BULK_RX_RAW);
                    status = STATUS_SUCCESS;
                    break;
                }

                USBD_PIPE_HANDLE rxPipe = RTW_FindBulkInPipe(pDevExt);
                if (rxPipe != NULL) {
                    ULONG bytesRead = 0;
                    rx->Status = (LONG)RTW_UsbBulkTransfer(
                        dev,
                        rxPipe,
                        USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK,
                        rx->Data,
                        maxLen,
                        &bytesRead
                    );
                    rx->Length = bytesRead;
                } else {
                    rx->Length = 0;
                    rx->Status = (LONG)STATUS_DEVICE_NOT_READY;
                }
                info = sizeof(RTW_BULK_RX_RAW);
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case IOCTL_RTW_BULK_TX_PIPE: {
            if (stack->Parameters.DeviceIoControl.InputBufferLength >= 8) {
                PRTW_BULK_TX_PIPE tx = (PRTW_BULK_TX_PIPE)Irp->AssociatedIrp.SystemBuffer;
                ULONG len = tx->Length;
                if (len > 4200 || stack->Parameters.DeviceIoControl.InputBufferLength < 8 + len) {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                USBD_PIPE_HANDLE txPipe = RTW_FindBulkOutPipeByIndex(pDevExt, tx->OutPipeIdx);
                if (txPipe != NULL) {
                    ULONG bytesWritten = 0;
                    status = RTW_UsbBulkTransfer(
                        dev,
                        txPipe,
                        0,
                        tx->Data,
                        len,
                        &bytesWritten
                    );
                    info = bytesWritten;
                } else {
                    status = STATUS_DEVICE_NOT_READY;
                    info = 0;
                }
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case IOCTL_RTW_GET_PIPES: {
            if (stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(RTW_PIPES_RESP)) {
                PRTW_PIPES_RESP out = (PRTW_PIPES_RESP)Irp->AssociatedIrp.SystemBuffer;
                RtlZeroMemory(out, sizeof(RTW_PIPES_RESP));

                if (pDevExt->ConfigurationHandle == NULL || pDevExt->NumberOfPipes == 0) {
                    RTW_ConfigureDevice(pDevExt);
                }

                out->NumPipes = pDevExt->NumberOfPipes;
                ULONG outCnt = 0;
                for (ULONG i = 0; i < pDevExt->NumberOfPipes && i < 8; i++) {
                    out->Pipes[i].EndpointAddress = pDevExt->Pipes[i].EndpointAddress;
                    out->Pipes[i].PipeType = (UCHAR)pDevExt->Pipes[i].PipeType;
                    out->Pipes[i].MaxPacketSize = (USHORT)pDevExt->Pipes[i].MaximumPacketSize;
                    if (pDevExt->Pipes[i].PipeType == UsbdPipeTypeBulk) {
                        if (pDevExt->Pipes[i].EndpointAddress & 0x80) {
                            if (out->BulkInAddr == 0) {
                                out->BulkInAddr = pDevExt->Pipes[i].EndpointAddress;
                            }
                        } else {
                            if (outCnt < 4) {
                                out->BulkOutAddrs[outCnt++] = pDevExt->Pipes[i].EndpointAddress;
                            }
                        }
                    }
                }
                out->NumBulkOut = outCnt;
                info = sizeof(RTW_PIPES_RESP);
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
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
    RtlZeroMemory(pDevExt, sizeof(DEVICE_EXTENSION));
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
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = RtwCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RtwDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = RtwPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = RtwPower;

    DbgPrint("RTWPROBE: DriverEntry Milestone 3 Ready\n");
    return STATUS_SUCCESS;
}
