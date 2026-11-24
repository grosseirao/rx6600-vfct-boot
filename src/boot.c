#include <Uefi.h>

#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#include "vfct.h"

STATIC EFI_GUID SimpleFsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
STATIC EFI_GUID DevicePathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
STATIC CONST CHAR16 WindowsBootPath[] = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

STATIC EFI_STATUS WindowsVolumePath(EFI_BOOT_SERVICES *Boot, EFI_HANDLE Volume,
                                    EFI_DEVICE_PATH_PROTOCOL **Path, UINTN *PrefixSize)
{
    EFI_STATUS Status = Boot->HandleProtocol(Volume, &DevicePathGuid, (VOID **)Path);
    if (EFI_ERROR(Status)) return Status;

    *PrefixSize = 0;
    BOOLEAN MatchesPartition = FALSE;
    BOOLEAN Nvme = FALSE;
    while (TRUE) {
        if (*PrefixSize > 4096 - sizeof **Path) return EFI_COMPROMISED_DATA;
        CONST UINT8 *Node = (CONST UINT8 *)*Path + *PrefixSize;
        UINTN Length = Node[2] | ((UINTN)Node[3] << 8);
        if (Length < sizeof **Path || Length > 4096 - *PrefixSize) return EFI_COMPROMISED_DATA;
        if (Node[0] == END_DEVICE_PATH_TYPE) {
            if (Node[1] != END_ENTIRE_DEVICE_PATH_SUBTYPE || Length != sizeof **Path) {
                return EFI_UNSUPPORTED;
            }
            break;
        }
        MatchesPartition = FALSE;
        if (Node[0] == MESSAGING_DEVICE_PATH && Node[1] == MSG_NVME_NAMESPACE_DP &&
            Length == sizeof(NVME_NAMESPACE_DEVICE_PATH)) {
            Nvme = TRUE;
        }
        if (Node[0] == MEDIA_DEVICE_PATH && Node[1] == MEDIA_HARDDRIVE_DP &&
            Length == sizeof(HARDDRIVE_DEVICE_PATH)) {
            CONST HARDDRIVE_DEVICE_PATH *Disk = (CONST HARDDRIVE_DEVICE_PATH *)Node;
            MatchesPartition = Disk->SignatureType == SIGNATURE_TYPE_GUID;
            MatchesPartition = MatchesPartition && Nvme;
        }
        *PrefixSize += Length;
    }
    return MatchesPartition ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC EFI_STATUS LoadWindowsFromPath(EFI_HANDLE Image, EFI_BOOT_SERVICES *Boot,
                                       EFI_DEVICE_PATH_PROTOCOL *Path, UINTN PrefixSize,
                                       EFI_HANDLE *Windows)
{
    UINTN FileNodeSize = sizeof *Path + sizeof WindowsBootPath;
    UINTN FullSize = PrefixSize + FileNodeSize + sizeof *Path;
    UINT8 *FullPath = NULL;
    EFI_STATUS Status = Boot->AllocatePool(EfiBootServicesData, FullSize, (VOID **)&FullPath);
    if (EFI_ERROR(Status)) return Status;
    Boot->CopyMem(FullPath, Path, PrefixSize);
    UINT8 *File = FullPath + PrefixSize;
    File[0] = MEDIA_DEVICE_PATH;
    File[1] = MEDIA_FILEPATH_DP;
    File[2] = (UINT8)FileNodeSize;
    File[3] = (UINT8)(FileNodeSize >> 8);
    Boot->CopyMem(File + sizeof *Path, (VOID *)WindowsBootPath, sizeof WindowsBootPath);
    Boot->CopyMem(File + FileNodeSize, (UINT8 *)Path + PrefixSize, sizeof *Path);
    Status = Boot->LoadImage(FALSE, Image, (EFI_DEVICE_PATH_PROTOCOL *)FullPath, NULL, 0, Windows);
    Boot->FreePool(FullPath);
    return Status;
}

STATIC EFI_STATUS LoadWindows(EFI_HANDLE Image, EFI_BOOT_SERVICES *Boot,
                               EFI_HANDLE *Windows)
{
    EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
    UINTN PrefixSize = 0;
    EFI_HANDLE *Volumes = NULL;
    UINTN Count = 0;
    EFI_STATUS Status = Boot->LocateHandleBuffer(ByProtocol, &SimpleFsGuid, NULL,
                                                  &Count, &Volumes);
    if (EFI_ERROR(Status)) return Status;
    for (UINTN Index = 0; Index < Count; ++Index) {
        EFI_DEVICE_PATH_PROTOCOL *Candidate;
        UINTN CandidateSize;
        if (EFI_ERROR(WindowsVolumePath(Boot, Volumes[Index], &Candidate, &CandidateSize))) {
            continue;
        }
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem = NULL;
        Status = Boot->HandleProtocol(Volumes[Index], &SimpleFsGuid, (VOID **)&FileSystem);
        if (EFI_ERROR(Status)) break;
        EFI_FILE_PROTOCOL *Root = NULL;
        Status = FileSystem->OpenVolume(FileSystem, &Root);
        if (EFI_ERROR(Status)) break;
        EFI_FILE_PROTOCOL *File = NULL;
        Status = Root->Open(Root, &File, (CHAR16 *)WindowsBootPath, EFI_FILE_MODE_READ, 0);
        Root->Close(Root);
        if (Status == EFI_NOT_FOUND) {
            Status = EFI_SUCCESS;
            continue;
        }
        if (EFI_ERROR(Status)) break;
        File->Close(File);
        if (Path) {
            Status = EFI_NO_MAPPING;
            break;
        }
        Path = Candidate;
        PrefixSize = CandidateSize;
    }
    Boot->FreePool(Volumes);
    if (EFI_ERROR(Status)) return Status;
    if (!Path) return EFI_NOT_FOUND;
    return LoadWindowsFromPath(Image, Boot, Path, PrefixSize, Windows);
}

STATIC EFI_STATUS Finish(EFI_SYSTEM_TABLE *System, CHAR16 *Message,
                         EFI_STATUS Status)
{
    System->ConOut->OutputString(System->ConOut, Message);
    return Status;
}

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE Image, EFI_SYSTEM_TABLE *System)
{
    EFI_BOOT_SERVICES *Boot = System->BootServices;
    EFI_ACPI_SUPPORT_PROTOCOL *Support = NULL;
    EFI_STATUS Status = Boot->LocateProtocol(FrameworkGuidAddress, NULL, (VOID **)&Support);
    if (EFI_ERROR(Status)) {
        return Finish(System, L"AMI ACPI Support protocol unavailable. No changes.\r\n", Status);
    }
    BOOLEAN Exact;
    EFI_ACPI_TABLE_VERSION Version;
    UINTN Key;
    Status = FindFrameworkVfct(Support, Boot, OriginalVfct, &Exact, &Key, &Version);
    if (EFI_ERROR(Status) || !Exact) {
        return Finish(System, L"AMI VFCT differs from the captured original. No changes.\r\n", EFI_ABORTED);
    }
    EFI_HANDLE Windows = NULL;
    Status = LoadWindows(Image, Boot, &Windows);
    if (EFI_ERROR(Status)) {
        return Finish(System, L"Expected Windows Boot Manager could not be loaded. No changes.\r\n", Status);
    }
    Status = ReplaceFrameworkVfct(Support, Boot, &Key, Version, TrialVfct);
    if (!EFI_ERROR(Status)) {
        Finish(System, L"Verified AMI trial VFCT: 1050 mV. Starting Windows.\r\n", EFI_SUCCESS);
        Boot->StartImage(Windows, NULL, NULL);
    } else {
        Boot->UnloadImage(Windows);
    }
    Status = RestoreFrameworkVfct(Support, Boot, Version);
    if (!EFI_ERROR(Status)) {
        return Finish(System, L"Trial ended. Original VFCT restored and verified.\r\n", EFI_ABORTED);
    }
    return Finish(System, L"Original VFCT could not be verified. Reboot now.\r\n", EFI_ABORTED);
}
