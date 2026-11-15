#!/usr/bin/env python3
"""Patch Mu-Silicium SEC to preserve entry x0 (ABL FDT pointer) in a GUID HOB
matching UniXNURamDiskDxe's mEntryArgHobGuid (DAEE82E6-8090-4E72-BFB9-78C13CD4E359).
"""
import argparse
from pathlib import Path


def replace_once(path: Path, old: str, new: str, marker: str) -> None:
    text = path.read_text()
    if marker in text:
        return
    if old not in text:
        raise SystemExit(f"patch-mu-sec-entryarg: expected pattern missing in {path}:\n{old}")
    path.write_text(text.replace(old, new, 1))


def apply_patches(uefi: Path, dry_run: bool = False) -> None:
    dec = uefi / "Silicon/Silicium/SiliciumPkg/SiliciumPkg.dec"
    inf = uefi / "Silicon/Silicium/SiliciumPkg/Sec/Sec.inf"
    entry_s = uefi / "Silicon/Silicium/SiliciumPkg/Sec/AArch64/ModuleEntryPoint.S"
    sec_c = uefi / "Silicon/Silicium/SiliciumPkg/Sec/Sec.c"

    edits = [
        (
            dec,
            "  gSiliciumPkgTokenSpaceGuid          = { 0x055720A6, 0xA739, 0x0047, { 0xBE, 0xC1, 0x67, 0xFC, 0x38, 0x34, 0x54, 0x53 } }",
            "  gSiliciumPkgTokenSpaceGuid          = { 0x055720A6, 0xA739, 0x0047, { 0xBE, 0xC1, 0x67, 0xFC, 0x38, 0x34, 0x54, 0x53 } }\n"
            "  gSiliciumEntryArgHobGuid            = { 0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 } }",
            "gSiliciumEntryArgHobGuid",
        ),
        (
            inf,
            "[Guids]\n  gArmMpCoreInfoGuid",
            "[Guids]\n  gArmMpCoreInfoGuid\n  gSiliciumEntryArgHobGuid",
            "gSiliciumEntryArgHobGuid",
        ),
        (
            entry_s,
            "ASM_FUNC (_ModuleEntryPoint)\n  // Do Platform Specific Initialization",
            "ASM_FUNC (_ModuleEntryPoint)\n  // Preserve incoming x0 (FDT pointer) across SEC initialization\n  mov   x19, x0\n\n  // Do Platform Specific Initialization",
            "mov   x19, x0",
        ),
        (
            entry_s,
            "  // Set Jump Location\n  ldr   x0, =ASM_PFX (SecEntry)\n\n  // Clear Frame Pointer\n  mov   x29, xzr\n\n  // Jump to C Code\n  blr   x0",
            "  // Set Jump Location and pass preserved entry x0\n  ldr   x1, =ASM_PFX (SecEntry)\n  mov   x0, x19\n\n  // Clear Frame Pointer\n  mov   x29, xzr\n\n  // Jump to C Code\n  blr   x1",
            "mov   x0, x19",
        ),
        (
            sec_c,
            "STATIC\nVOID\nSecMain (VOID)",
            "STATIC\nVOID\nSecMain (\n  IN UINT64 EntryArg0\n  )",
            "SecMain (\n  IN UINT64 EntryArg0",
        ),
        (
            sec_c,
            "  // Run Library Cconstructors\n  ProcessLibraryConstructorList ();",
            "  // Run Library Cconstructors\n  ProcessLibraryConstructorList ();\n\n  // Publish incoming x0 (FDT pointer) for UniXNURamDiskDxe\n  BuildGuidDataHob (&gSiliciumEntryArgHobGuid, &EntryArg0, sizeof (EntryArg0));",
            "gSiliciumEntryArgHobGuid",
        ),
        (
            sec_c,
            "VOID\nSecEntry (VOID)",
            "VOID\nSecEntry (\n  IN UINT64 EntryArg0\n  )",
            "SecEntry (\n  IN UINT64 EntryArg0",
        ),
        (
            sec_c,
            "  // Enter Main SEC Function\n  SecMain ();",
            "  // Enter Main SEC Function\n  SecMain (EntryArg0);",
            "SecMain (EntryArg0);",
        ),
        (
            sec_c,
            "STATIC\nEFI_STATUS\nInitializeMemory (VOID)",
            "STATIC\nUINT32\nFdtBe32 (\n  IN CONST UINT8  *Ptr\n  )\n{\n  return ((UINT32)Ptr[0] << 24) | ((UINT32)Ptr[1] << 16) | ((UINT32)Ptr[2] << 8) | (UINT32)Ptr[3];\n}\n\nSTATIC\nUINT64\nFdtBe64 (\n  IN CONST UINT8  *Ptr\n  )\n{\n  return ((UINT64)FdtBe32 (Ptr) << 32) | (UINT64)FdtBe32 (Ptr + 4);\n}\n\nSTATIC\nBOOLEAN\nFdtStrEq (\n  IN CONST CHAR8  *A,\n  IN CONST CHAR8  *B\n  )\n{\n  while (*A != '\\0' && *A == *B) {\n    A++;\n    B++;\n  }\n  return *A == *B;\n}\n\nSTATIC\nBOOLEAN\nFdtIsMemoryNode (\n  IN CONST CHAR8  *Name\n  )\n{\n  return (Name[0] == 'm' && Name[1] == 'e' && Name[2] == 'm' &&\n          Name[3] == 'o' && Name[4] == 'r' && Name[5] == 'y' &&\n          (Name[6] == '\\0' || Name[6] == '@'));\n}\n\nSTATIC\nVOID\nUpdateRamPoolFromFdt (\n  IN UINT64  FdtAddr\n  )\n{\n  CONST UINT8                   *Fdt;\n  UINT32                        TotalSize;\n  UINT32                        OffStruct;\n  UINT32                        OffStrings;\n  UINT32                        SizeStruct;\n  UINT32                        Pos;\n  UINT32                        EndStruct;\n  UINT32                        Depth;\n  UINT32                        AddrCells;\n  UINT32                        SizeCells;\n  BOOLEAN                       InMemory;\n  UINT64                        MaxEnd;\n  EFI_MEMORY_REGION_DESCRIPTOR  *Desc;\n  UINT8                         Count;\n  UINTN                         Index;\n\n  if ((FdtAddr == 0) || ((FdtAddr & 3ULL) != 0) || (FdtAddr < 0x40000000ULL) || (FdtAddr >= 0x240000000ULL)) {\n    return;\n  }\n\n  Fdt = (CONST UINT8 *)(UINTN)FdtAddr;\n  if (FdtBe32 (Fdt) != 0xD00DFEEDu) {\n    return;\n  }\n\n  TotalSize  = FdtBe32 (Fdt + 4);\n  OffStruct  = FdtBe32 (Fdt + 8);\n  OffStrings = FdtBe32 (Fdt + 12);\n  SizeStruct = FdtBe32 (Fdt + 36);\n  if ((TotalSize < 40) || (TotalSize > 0x1000000u) ||\n      (OffStruct < 40) || (OffStrings < 40) ||\n      (OffStruct >= TotalSize) || (OffStrings >= TotalSize) ||\n      (SizeStruct > TotalSize - OffStruct)) {\n    return;\n  }\n\n  Pos       = OffStruct;\n  EndStruct = OffStruct + SizeStruct;\n  Depth     = 0;\n  AddrCells = 2;\n  SizeCells = 2;\n  InMemory  = FALSE;\n  MaxEnd    = 0;\n\n  while (Pos + 4 <= EndStruct) {\n    UINT32  Token = FdtBe32 (Fdt + Pos);\n    Pos += 4;\n    if (Token == 1) {\n      CONST CHAR8  *Name = (CONST CHAR8 *)(Fdt + Pos);\n      UINT32       Len   = 0;\n      UINT32       Step;\n      while ((Pos + Len < EndStruct) && (Name[Len] != '\\0')) {\n        Len++;\n      }\n      if (Pos + Len >= EndStruct) {\n        break;\n      }\n      Step = (Len + 1u + 3u) & ~3u;\n      if (Step > EndStruct - Pos) {\n        break;\n      }\n      Pos += Step;\n      Depth++;\n      if (Depth == 2) {\n        InMemory = FdtIsMemoryNode (Name);\n      }\n    } else if (Token == 2) {\n      if (Depth == 2) {\n        InMemory = FALSE;\n      }\n      if (Depth > 0) {\n        Depth--;\n      }\n    } else if (Token == 3) {\n      UINT32       PropLen;\n      UINT32       NameOff;\n      UINT32       Aligned;\n      CONST CHAR8  *PropName;\n      CONST UINT8  *PropVal;\n      if (Pos + 8 > EndStruct) {\n        break;\n      }\n      PropLen = FdtBe32 (Fdt + Pos);\n      NameOff = FdtBe32 (Fdt + Pos + 4);\n      Pos    += 8;\n      if ((PropLen > EndStruct - Pos) || (NameOff >= TotalSize - OffStrings)) {\n        break;\n      }\n      PropName = (CONST CHAR8 *)(Fdt + OffStrings + NameOff);\n      PropVal  = Fdt + Pos;\n      Aligned  = (PropLen + 3u) & ~3u;\n      if (Aligned > EndStruct - Pos) {\n        break;\n      }\n      Pos += Aligned;\n      if ((Depth == 1) && (PropLen == 4)) {\n        if (FdtStrEq (PropName, \"#address-cells\")) {\n          AddrCells = FdtBe32 (PropVal);\n        } else if (FdtStrEq (PropName, \"#size-cells\")) {\n          SizeCells = FdtBe32 (PropVal);\n        }\n      } else if ((Depth == 2) && InMemory && FdtStrEq (PropName, \"reg\") &&\n                 ((AddrCells == 1) || (AddrCells == 2)) && ((SizeCells == 1) || (SizeCells == 2))) {\n        UINT32  EntryBytes = (AddrCells + SizeCells) * 4u;\n        UINT32  Offset;\n        for (Offset = 0; Offset + EntryBytes <= PropLen; Offset += EntryBytes) {\n          UINT64  Base = (AddrCells == 2) ? FdtBe64 (PropVal + Offset) : (UINT64)FdtBe32 (PropVal + Offset);\n          UINT64  Size = (SizeCells == 2) ? FdtBe64 (PropVal + Offset + AddrCells * 4u) : (UINT64)FdtBe32 (PropVal + Offset + AddrCells * 4u);\n          if ((Size > 0) && (Base >= 0x40000000ULL) && (Size <= 0x200000000ULL) && (Base + Size <= 0x240000000ULL)) {\n            if (Base + Size > MaxEnd) {\n              MaxEnd = Base + Size;\n            }\n          }\n        }\n      }\n    } else if (Token == 4) {\n      continue;\n    } else {\n      break;\n    }\n  }\n\n  MaxEnd &= ~0x1FFFFFULL;\n  if (MaxEnd <= 0x140000000ULL) {\n    return;\n  }\n\n  Desc  = NULL;\n  Count = 0;\n  GetMemoryMap (&Desc, &Count);\n  if (Desc == NULL) {\n    return;\n  }\n  for (Index = 0; Index < Count; Index++) {\n    if ((Desc[Index].Address == 0x80000000ULL) && (Desc[Index].HobOption == AddMem) &&\n        (MaxEnd - 0x80000000ULL > Desc[Index].Length)) {\n      Desc[Index].Length = MaxEnd - 0x80000000ULL;\n      break;\n    }\n  }\n}\n\nSTATIC\nEFI_STATUS\nInitializeMemory (VOID)",
            "UpdateRamPoolFromFdt",
        ),
        (
            sec_c,
            "  // Initialize Memory\n  Status = InitializeMemory ();",
            "  // Extend RAM Pool 2 from ABL FDT /memory before InitializeMemory configures MMU and HOBs\n  UpdateRamPoolFromFdt (EntryArg0);\n\n  // Initialize Memory\n  Status = InitializeMemory ();",
            "UpdateRamPoolFromFdt (EntryArg0);",
        ),
    ]

    for path, old, new, marker in edits:
        text = path.read_text()
        if marker in text:
            continue
        if old not in text:
            raise SystemExit(f"patch-mu-sec-entryarg: expected pattern missing in {path}:\n{old}")
        if not dry_run:
            replace_once(path, old, new, marker)

    # Keep DarwinBoot Video[1] (v_display) at 1 so NativeLegacyDisplay recognizes the primary GOP framebuffer.
    bv_found = 0
    for root_dir in [uefi, uefi.parent]:
        if root_dir.exists():
            for bv in root_dir.rglob("BootVideo.h"):
                bv_found += 1
                bv_text = bv.read_text()
                if "Video[1] = 0;" in bv_text:
                    if not dry_run:
                        bv.write_text(bv_text.replace("Video[1] = 0;", "Video[1] = 1;"))
                    print(f"{'Verified' if dry_run else 'Restored'} DarwinBoot BootVideo.h at {bv} (v_display = 1 for primary GOP display)")
                elif "Video[1] = 1;" in bv_text:
                    print(f"Verified DarwinBoot BootVideo.h at {bv} (v_display = 1 for primary GOP display)")
    if bv_found == 0:
        raise SystemExit("patch-mu-sec-entryarg: no BootVideo.h found! Ensure UniXNU submodules are checked out.")

    # Allow DarwinBoot AllocateWindow to fall back cleanly to the largest contiguous
    # free EfiConventionalMemory run when the requested owned_memory_size exceeds
    # physical RAM on smaller-RAM SKUs (e.g. 4 GiB / 6 GiB) or higher DXE usage.
    db_found = 0
    for root_dir in [uefi, uefi.parent]:
        if root_dir.exists():
            for db in root_dir.rglob("DarwinBoot.c"):
                db_found += 1
                db_text = db.read_text()
                if "MaxAvail" in db_text:
                    print(f"Verified DarwinBoot adaptive window in {db}")
                    continue
                sig_old = "AllocateWindow (KERNEL *Kernel, UINT64 WindowSize, EFI_PHYSICAL_ADDRESS *Address)\n{"
                sig_new = "AllocateWindow (KERNEL *Kernel, UINT64 *WindowSizePtr, EFI_PHYSICAL_ADDRESS *Address)\n{\n  UINT64                WindowSize = *WindowSizePtr;"
                fallback_old = "  if (EFI_ERROR (Status)) {\n    CHAR16 Line[96];"
                fallback_new = (
                    "  if (EFI_ERROR (Status) && (BestBytes > BLOCK_SIZE) && (Kernel != NULL)) {\n"
                    "    UINT64                BestEnd = BestStart + BestBytes;\n"
                    "    UINT64                MaxAvail;\n"
                    "    UINTN                 Step;\n"
                    "    EFI_PHYSICAL_ADDRESS  TryAddr;\n"
                    "    Candidate = ALIGN_VALUE (BestStart, BLOCK_SIZE) + (Kernel->Base & (BLOCK_SIZE - 1));\n"
                    "    if ((Candidate >= BestStart) && (BestEnd > Candidate)) {\n"
                    "      MaxAvail = (BestEnd - Candidate) & ~(BLOCK_SIZE - 1);\n"
                    "      if (MaxAvail > WindowSize) { MaxAvail = WindowSize; }\n"
                    "      for (Step = 0; (Step < 16) && (MaxAvail >= 0x10000000ULL); Step++) {\n"
                    "        TryAddr = Candidate;\n"
                    "        Status = gBS->AllocatePages (AllocateAddress, EfiLoaderCode, EFI_SIZE_TO_PAGES (MaxAvail), &TryAddr);\n"
                    "        if (!EFI_ERROR (Status)) {\n"
                    "          *Address       = TryAddr;\n"
                    "          *WindowSizePtr = MaxAvail;\n"
                    "          WindowSize     = MaxAvail;\n"
                    "          break;\n"
                    "        }\n"
                    "        if (MaxAvail <= BLOCK_SIZE) { break; }\n"
                    "        MaxAvail -= BLOCK_SIZE;\n"
                    "      }\n"
                    "    }\n"
                    "  }\n"
                    "  if (EFI_ERROR (Status)) {\n    CHAR16 Line[96];"
                )
                call_old = "    Status = AllocateWindow (&Kernel, WindowSize, &Address);"
                call_new = "    Status = AllocateWindow (&Kernel, &WindowSize, &Address);"
                for pat in (sig_old, fallback_old, call_old):
                    if pat not in db_text:
                        raise SystemExit(f"patch-mu-sec-entryarg: expected pattern missing in {db}:\n{pat}")
                if not dry_run:
                    db_text = db_text.replace(sig_old, sig_new, 1).replace(fallback_old, fallback_new, 1).replace(call_old, call_new, 1)
                    db.write_text(db_text)
                print(f"{'Verified' if dry_run else 'Patched'} DarwinBoot adaptive window in {db}")
    if db_found == 0:
        raise SystemExit("patch-mu-sec-entryarg: no DarwinBoot.c found!")

    print(f"{'Verified' if dry_run else 'Patched'} Mu-Silicium SEC entry-arg HOB in {uefi}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("uefi", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    apply_patches(args.uefi, dry_run=args.dry_run)


if __name__ == "__main__":
    main()
