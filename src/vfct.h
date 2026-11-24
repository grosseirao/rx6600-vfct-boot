#ifndef RX6600_VFCT_H
#define RX6600_VFCT_H

#include <Uefi.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/AcpiSupport.h>

#include "VfctData.h"

STATIC EFI_GUID FrameworkGuid = EFI_ACPI_SUPPORT_GUID;
STATIC EFI_GUID * volatile FrameworkGuidAddress = &FrameworkGuid;

STATIC BOOLEAN Matches(CONST EFI_ACPI_SDT_HEADER *Table, CONST UINT8 *Data)
{
    CONST UINT8 *Bytes = (CONST UINT8 *)Table;
    if (Table->Length != sizeof OriginalVfct) {
        return FALSE;
    }
    for (UINTN Index = 0; Index < sizeof OriginalVfct; ++Index) {
        if (Bytes[Index] != Data[Index]) {
            return FALSE;
        }
    }
    return TRUE;
}

STATIC EFI_STATUS FindFrameworkVfct(EFI_ACPI_SUPPORT_PROTOCOL *Support,
                                    EFI_BOOT_SERVICES *Boot, CONST UINT8 *Expected,
                                    BOOLEAN *Exact, UINTN *FoundKey,
                                    EFI_ACPI_TABLE_VERSION *FoundVersion)
{
    BOOLEAN Found = FALSE;
    *Exact = FALSE;
    for (INTN Index = 0; Index < 256; ++Index) {
        VOID *Entry = NULL;
        UINTN Key;
        EFI_ACPI_TABLE_VERSION Version;
        EFI_STATUS Status = Support->GetAcpiTable(Support, Index, &Entry, &Version, &Key);
        if (Status == EFI_NOT_FOUND) return Found ? EFI_SUCCESS : EFI_NOT_FOUND;
        if (EFI_ERROR(Status)) return Status;
        EFI_ACPI_SDT_HEADER *Table = (EFI_ACPI_SDT_HEADER *)Entry;
        if (Table && Table->Signature == SIGNATURE_32('V','F','C','T')) {
            if (Found || !Key || !Version) {
                Boot->FreePool(Table);
                return EFI_COMPROMISED_DATA;
            }
            Found = TRUE;
            *Exact = Matches(Table, Expected);
            *FoundKey = Key;
            *FoundVersion = Version;
        }
        if (Entry) Boot->FreePool(Entry);
    }
    return EFI_ABORTED;
}

STATIC EFI_STATUS ReplaceFrameworkVfct(EFI_ACPI_SUPPORT_PROTOCOL *Support, EFI_BOOT_SERVICES *Boot,
                                       UINTN *Key, EFI_ACPI_TABLE_VERSION Version,
                                       CONST UINT8 *Data)
{
    EFI_STATUS Status = Support->SetAcpiTable(Support, (VOID *)Data, TRUE, Version, Key);
    if (EFI_ERROR(Status)) return Status;
    Status = Support->PublishTables(Support, Version);
    if (EFI_ERROR(Status)) return Status;
    BOOLEAN Exact;
    UINTN CheckedKey;
    EFI_ACPI_TABLE_VERSION CheckedVersion;
    Status = FindFrameworkVfct(Support, Boot, Data, &Exact, &CheckedKey, &CheckedVersion);
    if (EFI_ERROR(Status)) return Status;
    return CheckedKey == *Key && CheckedVersion == Version && Exact
        ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}

STATIC EFI_STATUS RestoreFrameworkVfct(EFI_ACPI_SUPPORT_PROTOCOL *Support,
                                      EFI_BOOT_SERVICES *Boot, EFI_ACPI_TABLE_VERSION Version)
{
    BOOLEAN Exact;
    UINTN Key;
    EFI_ACPI_TABLE_VERSION CurrentVersion;
    EFI_STATUS Status = FindFrameworkVfct(Support, Boot, OriginalVfct, &Exact, &Key, &CurrentVersion);
    /* AMI removes the old record before installing a replacement. A failed
       install can therefore leave no record, which requires a fresh key. */
    if (Status == EFI_NOT_FOUND) {
        Key = 0;
    } else if (EFI_ERROR(Status) || CurrentVersion != Version) {
        return EFI_COMPROMISED_DATA;
    } else if (Exact) {
        return EFI_SUCCESS;
    }
    return ReplaceFrameworkVfct(Support, Boot, &Key, Version, OriginalVfct);
}

#endif
