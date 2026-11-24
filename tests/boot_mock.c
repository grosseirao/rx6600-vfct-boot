#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#undef NULL
#include "boot.c"
#include "driver.c"

static EFI_ACPI_SUPPORT_PROTOCOL Framework;
static UINT8 Active[sizeof OriginalVfct];
static UINTN ActiveKey;
static unsigned Installs;
static int Duplicate;
static int RejectTrial;
static int CorruptTrial;

#define HARDWARE_PREFIX sizeof(NVME_NAMESPACE_DEVICE_PATH)
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL FileSystem;
static EFI_FILE_PROTOCOL RootFile, BootFile;
static int WindowsCopies;
static int UsbDisk;
#define PARTITION_PATH_SIZE (HARDWARE_PREFIX + sizeof(HARDDRIVE_DEVICE_PATH))
static UINT8 DevicePath[PARTITION_PATH_SIZE + sizeof(EFI_DEVICE_PATH_PROTOCOL)];
static EFI_LOADED_IMAGE_PROTOCOL LoadedImage;
static unsigned WindowsStarts;


static EFI_STATUS EFIAPI HandleProtocol(EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Result)
{
    (void)Handle;
    if (!memcmp(Guid, &DevicePathGuid, sizeof *Guid)) *Result = DevicePath;
    else if (!memcmp(Guid, &SimpleFsGuid, sizeof *Guid)) *Result = &FileSystem;
    else return EFI_NOT_FOUND;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI Allocate(EFI_MEMORY_TYPE Type, UINTN Size, VOID **Result)
{
    (void)Type;
    *Result = malloc(Size);
    return *Result ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS EFIAPI Free(VOID *Pointer) { free(Pointer); return EFI_SUCCESS; }
static VOID EFIAPI Copy(VOID *Out, VOID *In, UINTN Size) { memcpy(Out, In, Size); }

static EFI_STATUS EFIAPI LocateVolumes(EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID *Guid,
                                       VOID *Key, UINTN *Count, EFI_HANDLE **Volumes)
{
    if (Type != ByProtocol || Key || memcmp(Guid, &SimpleFsGuid, sizeof *Guid)) abort();
    *Count = WindowsCopies == 2 ? 2 : 1;
    EFI_STATUS Status = Allocate(EfiBootServicesData, *Count * sizeof **Volumes, (VOID **)Volumes);
    if (EFI_ERROR(Status)) return Status;
    memset(*Volumes, 0, *Count * sizeof **Volumes);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs,
                                    EFI_FILE_PROTOCOL **Root)
{
    if (Fs != &FileSystem || UsbDisk) abort();
    *Root = &RootFile;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI OpenFile(EFI_FILE_PROTOCOL *Root, EFI_FILE_PROTOCOL **File,
                                  CHAR16 *Name, UINT64 Mode, UINT64 Attributes)
{
    if (Root != &RootFile || memcmp(Name, WindowsBootPath, sizeof WindowsBootPath) ||
        Mode != EFI_FILE_MODE_READ || Attributes) abort();
    if (!WindowsCopies) return EFI_NOT_FOUND;
    *File = &BootFile;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI CloseFile(EFI_FILE_PROTOCOL *File)
{
    return File == &BootFile || File == &RootFile ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI Load(BOOLEAN Policy, EFI_HANDLE Parent, EFI_DEVICE_PATH_PROTOCOL *Path,
                              VOID *Buffer, UINTN Size, EFI_HANDLE *Result)
{
    (void)Parent;
    if (Policy || Buffer || Size || !ActiveKey || Installs) abort();
    const UINT8 *File = (const UINT8 *)Path + PARTITION_PATH_SIZE;
    if (memcmp(Path, DevicePath, PARTITION_PATH_SIZE) ||
        File[0] != MEDIA_DEVICE_PATH || File[1] != MEDIA_FILEPATH_DP ||
        File[2] != sizeof WindowsBootPath + 4 || File[3] ||
        memcmp(File + 4, WindowsBootPath, sizeof WindowsBootPath) ||
        memcmp(File + 4 + sizeof WindowsBootPath,
               DevicePath + PARTITION_PATH_SIZE, 4)) abort();
    *Result = &LoadedImage;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI Start(EFI_HANDLE Image, UINTN *ExitSize, CHAR16 **ExitData)
{
    (void)ExitSize;
    (void)ExitData;
    if (Image != &LoadedImage || memcmp(Active, TrialVfct, sizeof Active)) abort();
    ++WindowsStarts;
    return EFI_LOAD_ERROR;
}

static EFI_STATUS EFIAPI Unload(EFI_HANDLE Image) {
    return Image == &LoadedImage ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI GetTable(UINTN Index, EFI_ACPI_SDT_HEADER **Table,
                                  EFI_ACPI_TABLE_VERSION *Version, UINTN *Key)
{
    if (!ActiveKey || Index > (UINTN)Duplicate) return EFI_NOT_FOUND;
    *Table = (EFI_ACPI_SDT_HEADER *)Active;
    *Version = EFI_ACPI_TABLE_VERSION_2_0;
    *Key = ActiveKey + Index;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI FrameworkGet(EFI_ACPI_SUPPORT_PROTOCOL *Protocol, INTN Index,
                                      VOID **Table, EFI_ACPI_TABLE_VERSION *Version, UINTN *Key)
{
    (void)Protocol;
    EFI_ACPI_SDT_HEADER *Header = NULL;
    EFI_STATUS Status = GetTable((UINTN)Index, &Header, Version, Key);
    *Table = NULL;
    if (!EFI_ERROR(Status)) {
        *Table = malloc(Header->Length);
        if (!*Table) return EFI_OUT_OF_RESOURCES;
        memcpy(*Table, Header, Header->Length);
    }
    return Status;
}

static EFI_STATUS EFIAPI FrameworkSet(EFI_ACPI_SUPPORT_PROTOCOL *Protocol, VOID *Buffer,
                                      BOOLEAN Checksum, EFI_ACPI_TABLE_VERSION Version, UINTN *Key)
{
    (void)Protocol;
    if (*Key != ActiveKey || !Checksum || Version != EFI_ACPI_TABLE_VERSION_2_0) abort();
    ++Installs;
    ActiveKey = 0;
    if (RejectTrial && Installs == 1) return EFI_OUT_OF_RESOURCES;
    memcpy(Active, Buffer, sizeof Active);
    if (CorruptTrial && Installs == 1) Active[100] ^= 1;
    *Key = ActiveKey = 20 + Installs;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI FrameworkPublish(EFI_ACPI_SUPPORT_PROTOCOL *Protocol,
                                          EFI_ACPI_TABLE_VERSION Version)
{
    (void)Protocol;
    return Version == EFI_ACPI_TABLE_VERSION_2_0 ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid, VOID *Registration, VOID **Result)
{
    (void)Registration;
    if (memcmp(Guid, &FrameworkGuid, sizeof *Guid) == 0) *Result = &Framework;
    else return EFI_NOT_FOUND;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI Print(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Out, CHAR16 *Text)
{
    (void)Out;
    while (*Text) putchar((char)*Text++);
    return EFI_SUCCESS;
}

static EFI_EVENT_NOTIFY Notify;
static CONST VOID *NotifyContext;

static EFI_STATUS EFIAPI CreateEvent(UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Callback,
                                    CONST VOID *Context, CONST EFI_GUID *Group, EFI_EVENT *Event)
{
    if (Type != EVT_NOTIFY_SIGNAL || Tpl != TPL_CALLBACK ||
        memcmp(Group, &ReadyToBoot, sizeof *Group)) abort();
    Notify = Callback;
    NotifyContext = Context;
    *Event = &Framework;
    return EFI_SUCCESS;
}

int main(void)
{
    EFI_BOOT_SERVICES Boot = {0};
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Out = {0};
    EFI_SYSTEM_TABLE System = {0};
    Boot.LocateProtocol = Locate;
    Out.OutputString = Print;
    System.BootServices = &Boot;
    System.ConOut = &Out;
    Framework.GetAcpiTable = FrameworkGet;
    Framework.SetAcpiTable = FrameworkSet;
    Framework.PublishTables = FrameworkPublish;
    Boot.HandleProtocol = HandleProtocol;
    Boot.AllocatePool = Allocate;
    Boot.FreePool = Free;
    Boot.CopyMem = Copy;
    Boot.LoadImage = Load;
    Boot.StartImage = Start;
    Boot.UnloadImage = Unload;
    Boot.CreateEventEx = CreateEvent;
    Boot.LocateHandleBuffer = LocateVolumes;
    FileSystem.OpenVolume = OpenVolume;
    RootFile.Open = OpenFile;
    RootFile.Close = BootFile.Close = CloseFile;
    const int Cases = 8;
    for (int Case = 0; Case < Cases; ++Case) {
        memcpy(Active, OriginalVfct, sizeof Active);
        ActiveKey = 10;
        Installs = 0;
        Duplicate = Case == 1;
        RejectTrial = Case == 2;
        CorruptTrial = Case == 3;
        if (Case == 4) Active[100] ^= 1;
        WindowsStarts = 0;

        HARDDRIVE_DEVICE_PATH Disk = {0};
        Disk.Header.Type = MEDIA_DEVICE_PATH;
        Disk.Header.SubType = MEDIA_HARDDRIVE_DP;
        Disk.Header.Length[0] = sizeof Disk;
        Disk.SignatureType = SIGNATURE_TYPE_GUID;
        WindowsCopies = Case == 6 ? 2 : Case == 5 ? 0 : 1;
        UsbDisk = Case == 7;
        NVME_NAMESPACE_DEVICE_PATH Nvme = {0};
        Nvme.Header.Type = MESSAGING_DEVICE_PATH;
        Nvme.Header.SubType = UsbDisk ? MSG_USB_DP : MSG_NVME_NAMESPACE_DP;
        Nvme.Header.Length[0] = sizeof Nvme;
        memcpy(DevicePath, &Nvme, sizeof Nvme);
        Disk.Signature[0] = 0x77;
        memcpy(DevicePath + HARDWARE_PREFIX, &Disk, sizeof Disk);
        EFI_DEVICE_PATH_PROTOCOL End = {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {4,0}};
        memcpy(DevicePath + PARTITION_PATH_SIZE, &End, sizeof End);
        EFI_STATUS Status = EfiMain(NULL, &System);
        if (Case == 0 && (Status != EFI_ABORTED || Installs != 2 ||
            WindowsStarts != 1 || memcmp(Active, OriginalVfct, sizeof Active))) return 1;
        if (Case == 5 && (Status != EFI_NOT_FOUND || Installs || WindowsStarts)) return 5;
        if (Case == 6 && (Status != EFI_NO_MAPPING || Installs || WindowsStarts)) return 6;
        if (Case == 7 && (Status != EFI_NOT_FOUND || Installs || WindowsStarts)) return 7;
        if (Case > 0 && Case < 5 && Status != EFI_ABORTED) return 2;
        if ((Case == 1 || Case == 4) && (Installs || ActiveKey != 10)) return 3;
        if ((Case == 2 || Case == 3) && (Installs != 2 || !ActiveKey ||
            memcmp(Active, OriginalVfct, sizeof Active))) return 4;
    }
    puts("PASS: trial, duplicate, install failure, verification failure, wrong original.");
    puts("PASS: new ESP GUID, missing loader, ambiguous loader, USB rejected, rollback on loader return.");
    ActiveKey = 0;
    Installs = 0;
    if (EfiDriverMain(NULL, &System) != EFI_SUCCESS || !Notify || Installs) return 8;
    ActiveKey = 10;
    memcpy(Active, OriginalVfct, sizeof Active);
    Notify(&Framework, (VOID *)NotifyContext);
    if (Installs != 1 || memcmp(Active, TrialVfct, sizeof Active)) return 9;
    Notify(&Framework, (VOID *)NotifyContext);
    if (Installs != 1 || memcmp(Active, TrialVfct, sizeof Active)) return 10;
    ActiveKey = 10;
    Installs = 0;
    RejectTrial = 1;
    memcpy(Active, OriginalVfct, sizeof Active);
    Notify(&Framework, (VOID *)NotifyContext);
    if (Installs != 2 || !ActiveKey || memcmp(Active, OriginalVfct, sizeof Active)) return 11;
    puts("PASS: late VFCT publication, idempotent ReadyToBoot, driver rollback.");
    return 0;
}
