#include "vfct.h"
#include <Guid/EventGroup.h>

STATIC CONST EFI_GUID ReadyToBoot = EFI_EVENT_GROUP_READY_TO_BOOT;

STATIC VOID EFIAPI CorrectVfct(EFI_EVENT Event, VOID *Context)
{
    (VOID)Event;
    EFI_BOOT_SERVICES *Boot = Context;
    EFI_ACPI_SUPPORT_PROTOCOL *Support = NULL;
    EFI_STATUS Status = Boot->LocateProtocol(FrameworkGuidAddress, NULL, (VOID **)&Support);
    if (EFI_ERROR(Status)) return;

    BOOLEAN Exact;
    EFI_ACPI_TABLE_VERSION Version;
    UINTN Key;
    Status = FindFrameworkVfct(Support, Boot, OriginalVfct, &Exact, &Key, &Version);
    if (EFI_ERROR(Status) || !Exact) return;
    Status = ReplaceFrameworkVfct(Support, Boot, &Key, Version, TrialVfct);
    if (EFI_ERROR(Status)) RestoreFrameworkVfct(Support, Boot, Version);
}

EFI_STATUS EFIAPI EfiDriverMain(EFI_HANDLE Image, EFI_SYSTEM_TABLE *System)
{
    (VOID)Image;
    EFI_BOOT_SERVICES *Boot = System->BootServices;
    EFI_EVENT Event;
    EFI_STATUS Status = Boot->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, CorrectVfct,
                                             Boot, &ReadyToBoot, &Event);
    if (EFI_ERROR(Status)) return Status;

    /* The table may be published only after loading the driver from DriverOrder. */
    CorrectVfct(Event, Boot);
    return EFI_SUCCESS;
}
