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
    ]

    for path, old, new, marker in edits:
        text = path.read_text()
        if marker in text:
            continue
        if old not in text:
            raise SystemExit(f"patch-mu-sec-entryarg: expected pattern missing in {path}:\n{old}")
        if not dry_run:
            replace_once(path, old, new, marker)
    print(f"{'Verified' if dry_run else 'Patched'} Mu-Silicium SEC entry-arg HOB in {uefi}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("uefi", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    apply_patches(args.uefi, dry_run=args.dry_run)


if __name__ == "__main__":
    main()
