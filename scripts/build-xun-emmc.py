#!/usr/bin/env python3
"""Build XunEmmc (NativeUfs + NativeEmmcBlock : IOBlockStorageDevice) for UniXNU-xun.

Compiles drivers/XunEmmc/NativeEmmc.cpp against the prepared IOStorageFamily
headers, verifies NativeEmmcBlock's 190-slot vtable against the original
com.apple.iokit.IOStorageFamily ABI in the target iPadOS kernelcache, emits
original-bindings.S, and archives both:
  - build/xun-emmc/libXunEmmc.a
  - build/sheng-ufs/libShengUfs.a  (for upstream XNU_NATIVE_UFS=1 linkage)
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
sys.path.insert(0, str(ROOT / 'scripts'))
import native_prelink as np
from kernel_target import TRIPLE

ORIGINAL_KERNELCACHE_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
TARGET_ARCH_OBJ_DIR = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT'


def require(cond, msg):
    if not cond:
        raise SystemExit(f'build-xun-emmc: {msg}')


def read_slide_from_board():
    candidates = []
    if 'UNIXNU_DEVICE' in os.environ:
        candidates.append(Path(os.environ['UNIXNU_DEVICE']) / 'board.toml')
    candidates.append(ROOT / 'board.toml')
    for board_path in candidates:
        if board_path.is_file():
            m = re.search(r'^slide\s*=\s*"([^"]+)"', board_path.read_text(), re.M)
            if m:
                return int(m.group(1), 0)
    return -0x1000000


def _decode_im4p_kernelcache(im4p_path, out_path):
    sys.path.insert(0, str(ROOT / 'scripts'))
    from unixnu.ipsw import im4p_payload
    payload = im4p_payload(im4p_path.read_bytes())
    packed = out_path.with_suffix('.lzfse')
    fat_raw = out_path.with_suffix('.fat')
    packed.write_bytes(payload)
    try:
        if platform.system() == 'Darwin':
            cmd = ['/usr/bin/compression_tool', '-decode', '-a', 'lzfse', '-i', str(packed), '-o', str(fat_raw)]
        else:
            cmd = ['lzfse', '-decode', '-i', str(packed), '-o', str(fat_raw)]
        subprocess.run(cmd, check=True)
        data = fat_raw.read_bytes()
        if data[:4] == b'\xca\xfe\xba\xbe':
            count = struct.unpack('>I', data[4:8])[0]
            slices = [struct.unpack('>5I', data[8 + 20 * i:28 + 20 * i]) for i in range(count)]
            arm64 = [s for s in slices if s[0] == 0x0100000c]
            require(len(arm64) == 1, 'Expected one arm64 slice in kernelcache')
            _, _, offset, size, _ = arm64[0]
            data = data[offset:offset + size]
        require(data[:4] == b'\xcf\xfa\xed\xfe', 'Decompressed kernelcache is not arm64 Mach-O')
        require(hashlib.sha256(data).hexdigest() == ORIGINAL_KERNELCACHE_SHA256,
                'Decompressed kernelcache SHA-256 mismatch')
        out_path.write_bytes(data)
    finally:
        packed.unlink(missing_ok=True)
        fat_raw.unlink(missing_ok=True)


def find_original_kernelcache():
    candidates = [
        ROOT / '.cache/firmware/kernelcache.macho/kernelcache.release.ipad7.decompressed',
        *sorted(ROOT.glob('out/ipsw/*/kernelcache.decompressed')),
        *sorted(ROOT.parent.glob('ipsw/*/kernelcache.decompressed')),
        *sorted(ROOT.parents[1].glob('ipsw/*/kernelcache.decompressed')),
        *sorted(ROOT.glob('.cache/ipsw/*/kernelcache.decompressed')),
    ]
    for path in candidates:
        if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == ORIGINAL_KERNELCACHE_SHA256:
            return path

    # Auto-fetch and decode KernelCache if not yet cached in out/ipsw/.
    im4ps = [
        *sorted(ROOT.glob('out/ipsw/*/kernelcache.release.ipad7')),
        *sorted(ROOT.parent.glob('ipsw/*/kernelcache.release.ipad7')),
    ]
    if not im4ps and (ROOT / 'unixnu').is_file() and 'UNIXNU_DEVICE' in os.environ:
        subprocess.run(
            [sys.executable, str(ROOT / 'unixnu'), 'ipsw', 'fetch', 'KernelCache',
             '-D', os.environ['UNIXNU_DEVICE']],
            cwd=ROOT,
            check=True,
        )
        im4ps = [
            *sorted(ROOT.glob('out/ipsw/*/kernelcache.release.ipad7')),
            *sorted(ROOT.parent.glob('ipsw/*/kernelcache.release.ipad7')),
        ]
    if im4ps:
        out_kc = im4ps[0].parent / 'kernelcache.decompressed'
        _decode_im4p_kernelcache(im4ps[0], out_kc)
        return out_kc

    require(False, 'Original decompressed iPadOS kernelcache not found (run unixnu ipsw fetch)')


def extract_vtable_symbols(obj_bytes, mangled_class):
    om = np.macho(obj_bytes)
    require(om['kind'] == 1, 'Expected ordinary ARM64 MH_OBJECT')
    sycmd = next(at for cmd, at, size in om['commands'] if cmd == 2)
    symoff, count, stroff, _ = struct.unpack_from('<4I', obj_bytes, sycmd + 8)
    symbols = []
    for i in range(count):
        ix, kind, _, _, value = struct.unpack_from('<IBBHQ', obj_bytes, symoff + i * 16)
        name = obj_bytes[stroff + ix:obj_bytes.index(b'\0', stroff + ix)].decode()
        symbols.append(dict(name=name, value=value, kind=kind))
    vtable = next((s['value'] for s in symbols if s['name'] == '__ZTV' + mangled_class), None)
    require(vtable is not None, f'Missing vtable __ZTV{mangled_class}')
    following = [s['value'] for s in symbols if not s['kind'] & 0xe0 and (s['kind'] & 0x0e) == 0x0e and s['value'] > vtable]
    end = min(following) if following else None
    relocations = {}
    for seg in om['segments']:
        for sec in seg['sections']:
            if sec['va'] <= vtable < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<II', obj_bytes, sec['command_offset'] + 56)
                for i in range(rc):
                    address, word = struct.unpack_from('<II', obj_bytes, ro + i * 8)
                    if (word >> 27 & 1) and (word >> 28 == 0) and ((word >> 25 & 3) == 3):
                        relocations[sec['va'] + address] = symbols[word & 0xffffff]['name']
                if end is None or end > sec['va'] + sec['size']:
                    end = sec['va'] + sec['size']
    methods = []
    for i in range(190):
        at = vtable + 16 + i * 8
        if end is not None and at >= end:
            break
        if at not in relocations:
            break
        methods.append(relocations[at])
    return symbols, methods, relocations, vtable


def build_compile_cmd(record, source_path, output_path, extra_flags):
    arguments = record['arguments']
    cmd, idx = [], 0
    while idx < len(arguments):
        val = arguments[idx]
        if val == record['file']:
            cmd.append(str(source_path))
        elif val == '-o':
            cmd.extend(['-o', str(output_path)])
            idx += 1
        elif val in ('-MF', '-MJ'):
            idx += 1
        elif val == '-Xclang' and idx + 1 < len(arguments) and arguments[idx + 1].startswith('-W'):
            idx += 1
        elif not val.startswith('-W') and val not in ('-MD', '-MP'):
            cmd.append(val)
        idx += 1
    cmd.extend([
        '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-Wno-deprecated-declarations',
        '-Wno-c99-designator', '-Wno-reorder-init-list',
        '-Wno-unnecessary-virtual-specifier', '-Wno-deprecated-enum-enum-conversion',
        '-I', str(ROOT / '.cache/xnu-work/osfmk'),
        *extra_flags,
    ])
    return cmd


def resolve_storage_bindings(record, out_dir):
    audit_path = ROOT / 'build/storage-abi/interface-audit.json'
    if audit_path.is_file():
        audit = json.loads(audit_path.read_text())
        require(audit.get('method_slots') == 190 and audit.get('abstract_complete_destructors_trap'),
                'Invalid existing storage interface audit')
        possible = {r['reference_name']: int(r['candidate_target'], 16)
                    for r in audit['methods'] if not r['kernel_names']}
        possible.update({
            '__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': int(audit['original_protected_constructor'], 16),
            '__ZN20IOBlockStorageDeviceD2Ev': int(audit['original_base_destructor'], 16),
            '__ZN20IOBlockStorageDevice10gMetaClassE': int(audit['original_metaclass'], 16),
            '__ZTV20IOBlockStorageDevice': int(audit['vtable'], 16),
        })
        return audit['methods'], possible

    # Derive exact bindings from public refer/IOStorageFamily + original kernelcache + slide.
    source_cpp = ROOT / 'refer/IOStorageFamily/IOBlockStorageDevice.cpp'
    require(source_cpp.is_file(), f'Missing {source_cpp}')
    ref_obj = out_dir / 'IOBlockStorageDevice-reference.o'
    cmd = build_compile_cmd(record, source_cpp, ref_obj, ['-I', str(ROOT / 'build/storage-abi/include')])
    res = subprocess.run(cmd, cwd=record['directory'], capture_output=True, text=True)
    if res.returncode:
        raise SystemExit((res.stdout + res.stderr)[-3500:])
    _, methods, _, _ = extract_vtable_symbols(ref_obj.read_bytes(), '20IOBlockStorageDevice')
    require(len(methods) == 190 and methods.count('___cxa_pure_virtual') == 14,
            f'Unexpected IOBlockStorageDevice vtable shape: {len(methods)} slots')

    orig_path = find_original_kernelcache()
    kernel = orig_path.read_bytes()
    orig = np.macho(kernel)
    exports = np.exports(kernel, orig)
    by_name = {name: va for va, names in exports.items() for name in names}
    parent = orig['segments']
    info_seg = next(s for s in parent if s['name'] == '__PRELINK_INFO')
    info_off = np.backed_offset(parent, info_seg['va'], info_seg['filesize'], len(kernel))
    metadata = np.unserialize_xml(kernel[info_off:info_off + info_seg['filesize']])
    entry = next(e for e in metadata['_PrelinkInfoDictionary']
                 if e['CFBundleIdentifier'] == 'com.apple.iokit.IOStorageFamily')
    mod_hdr = np.backed_offset(parent, entry['_PrelinkExecutableLoadAddr'], 32, len(kernel))
    mod = np.macho(kernel, mod_hdr)
    const = next(s for s in mod['segments'] if s['name'] == '__DATA_CONST')
    text = next(s for s in mod['segments'] if s['name'] == '__TEXT_EXEC')

    def read_va(va, length):
        at = np.backed_offset(parent, va, length, len(kernel))
        return kernel[at:at + length]

    blob = read_va(const['va'], const['size'])
    found = []
    for at in range(0, len(blob) - (len(methods) + 2) * 8 + 1, 8):
        if blob[at:at + 16] != bytes(16):
            continue
        pointers = struct.unpack_from('<' + 'Q' * len(methods), blob, at + 16)
        if all(pointers[i] == by_name[name] for i, name in enumerate(methods) if name in by_name):
            found.append((const['va'] + at, pointers))
    require(len(found) == 1, f'Expected 1 IOBlockStorageDevice vtable match, found {len(found)}')
    table_va, pointers = found[0]
    for i, name in enumerate(methods):
        if name not in by_name:
            require(text['va'] <= pointers[i] < text['va'] + text['size'],
                    f'Owned IOBlockStorageDevice slot {i} outside IOStorageFamily __TEXT_EXEC')

    def relative_branch(address, insn):
        imm = insn & 0x3ffffff
        if imm & 0x2000000:
            imm -= 0x4000000
        return address + imm * 4

    def signed_adrp_page(address, insn):
        imm = ((insn >> 5 & 0x7ffff) << 2) | (insn >> 29 & 3)
        if imm & 0x100000:
            imm -= 0x200000
        return (address & ~4095) + imm * 4096

    constructors = []
    text_blob = read_va(text['va'], text['size'])
    for off in range(0, len(text_blob) - 32, 4):
        address = text['va'] + off
        words = struct.unpack_from('<8I', text_blob, off)
        if (words[:2] != (0xa9bf7bfd, 0x910003fd) or (words[2] & 0xfc000000) != 0x94000000 or
                (words[3] & 0x9f00001f) != 0x90000008 or (words[4] & 0xffc003ff) != 0x91000108 or
                words[5:] != (0xf9000008, 0xa8c17bfd, 0xd65f03c0)):
            continue
        stored = signed_adrp_page(address + 12, words[3]) + (words[4] >> 10 & 4095)
        if stored == table_va + 16:
            constructors.append(address)
    require(len(constructors) == 1, f'Expected 1 IOBlockStorageDevice constructor, found {len(constructors)}')

    destructor_va = pointers[0] - 4
    dtor_word = struct.unpack('<I', read_va(destructor_va, 4))[0]
    require((dtor_word & 0xfc000000) == 0x14000000, 'Unexpected IOBlockStorageDevice D2 destructor')

    meta_fn = pointers[7]
    a, add, ret = struct.unpack('<3I', read_va(meta_fn, 12))
    require((a & 0x9f00001f) == 0x90000000 and (add & 0xffc003ff) == 0x91000000 and ret == 0xd65f03c0,
            'Unexpected IOBlockStorageDevice metaclass getter')
    meta_va = signed_adrp_page(meta_fn, a) + (add >> 10 & 4095)

    slide = read_slide_from_board()
    possible = {}
    audit_methods = []
    for i, name in enumerate(methods):
        slid = pointers[i] + slide
        knames = exports.get(pointers[i], [])
        audit_methods.append(dict(slot=i, reference_name=name, candidate_target=hex(slid), kernel_names=knames))
        if (not knames or text['va'] <= pointers[i] < text['va'] + text['size']) and name != '___cxa_pure_virtual':
            possible[name] = slid
    possible.update({
        '__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': constructors[0] + slide,
        '__ZN20IOBlockStorageDeviceD2Ev': destructor_va + slide,
        '__ZN20IOBlockStorageDevice10gMetaClassE': meta_va + slide,
        '__ZTV20IOBlockStorageDevice': table_va + slide,
    })

    # Optional Pass 2 cross-check if prelinked kernel.macho already exists.
    ref_kernel = ROOT.parent / 'kernel/kernel.macho'
    if ref_kernel.is_file():
        rk = ref_kernel.read_bytes()
        rm = np.macho(rk)
        try:
            off = np.backed_offset(rm['segments'], table_va + slide + 16 + 7 * 8, 8, len(rk))
            slid_meta_fn = struct.unpack_from('<Q', rk, off)[0]
            require(slid_meta_fn == meta_fn + slide, 'Pass 2 kernel.macho vtable slide mismatch')
        except ValueError:
            pass

    return audit_methods, possible


def main():
    out = ROOT / 'build/xun-emmc'
    out.mkdir(parents=True, exist_ok=True)
    storage_include = ROOT / 'build/storage-abi/include'
    if not (storage_include / 'IOKit/storage/IOBlockStorageDevice.h').is_file():
        prep = ROOT / 'scripts/prepare-storage-headers.py'
        if prep.is_file():
            subprocess.run([sys.executable, str(prep)], cwd=ROOT, check=True)
    require((storage_include / 'IOKit/storage/IOBlockStorageDevice.h').is_file(),
            'Missing build/storage-abi/include IOBlockStorageDevice.h')

    cpo_path = TARGET_ARCH_OBJ_DIR / 'IOPlatformExpert.cpo.json'
    require(cpo_path.is_file(), f'Missing {cpo_path}')
    record = json.loads(cpo_path.read_text().strip().rstrip(','))

    audit_methods, possible = resolve_storage_bindings(record, out)

    source = ROOT / 'drivers/XunEmmc/NativeEmmc.cpp'
    obj = out / 'NativeEmmc.o'
    cmd = build_compile_cmd(
        record, source, obj,
        ['-I', str(storage_include), '-mgeneral-regs-only']
    )
    res = subprocess.run(cmd, cwd=record['directory'], capture_output=True, text=True)
    if res.returncode:
        raise SystemExit((res.stdout + res.stderr)[-6000:])

    symbols, compiled, _, _ = extract_vtable_symbols(obj.read_bytes(), '15NativeEmmcBlock')
    require(len(compiled) == 190 and '___cxa_pure_virtual' not in compiled,
            f'NativeEmmcBlock vtable invalid (len={len(compiled)})')

    demangled = subprocess.check_output(
        ['xcrun', 'c++filt'],
        input='\n'.join(n[1:] for n in [r['reference_name'] for r in audit_methods] + compiled) + '\n',
        text=True,
    ).splitlines()
    readable = dict(zip([r['reference_name'] for r in audit_methods] + compiled, demangled))
    overrides = 0
    for row, actual in zip(audit_methods, compiled):
        expected = row['reference_name']
        own = actual.startswith(('__ZN15NativeEmmcBlock', '__ZNK15NativeEmmcBlock'))
        if expected == '___cxa_pure_virtual':
            require(own, f'Missing pure virtual override at slot {row["slot"]}')
        elif actual != expected:
            require(own and (row['slot'] in (0, 1) or
                             readable[actual].split('::', 1)[1] == readable[expected].split('::', 1)[1]),
                    f'Concrete subclass dispatch mismatch at slot {row["slot"]}: {actual} vs {expected}')
        overrides += int(own)

    undefined = [s['name'] for s in symbols if s['kind'] == 1 and 'IOBlockStorageDevice' in s['name']]
    require(all(name in possible for name in undefined),
            'Unresolved IOBlockStorageDevice symbol: ' + ', '.join(n for n in undefined if n not in possible))
    require(not any(re.search(r'IOBlockStorageDeviceD[01]Ev$', n) for n in undefined),
            'Forbidden abstract complete/deleting IOBlockStorageDevice destructor call')

    bindings = {name: possible[name] for name in undefined}
    asm = out / 'original-bindings.S'
    asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in sorted(bindings.items())))
    subprocess.run(
        ['xcrun', 'clang', '-target', TRIPLE, '-Werror', '-Wno-incompatible-sysroot',
         '-c', str(asm), '-o', str(out / 'original-bindings.o')],
        check=True,
    )

    archive = out / 'libXunEmmc.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj), str(out / 'original-bindings.o')], check=True)

    # Upstream UniXNU build-xnu-4k-baseline.sh links build/sheng-ufs/libShengUfs.a when XNU_NATIVE_UFS=1.
    ufs_archive = ROOT / 'build/sheng-ufs/libShengUfs.a'
    ufs_archive.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(archive, ufs_archive)

    print(f'PASS: NativeEmmcBlock 190 slots ({overrides} overrides, {len(bindings)} IOBlockStorageDevice bindings)')
    print(archive)


if __name__ == '__main__':
    main()
