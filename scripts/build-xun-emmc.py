#!/usr/bin/env python3
"""XunEmmc (drivers/XunEmmc/NativeEmmc.cpp): build/xun-emmc/libXunEmmc.a,
the archive that CONFIG_XNU_NATIVE_EMMC links (CONFIG_XNU_NATIVE_EMMC_ARCHIVE)."""
import json, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
try:
    import native_prelink as np
except ImportError:
    np = None

sys.path.insert(0, str(ROOT / 'scripts'))
try:
    from kernel_target import TRIPLE
except ImportError:
    TRIPLE = 'arm64-apple-ios17.0'

cxx_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
out = ROOT / 'build/xun-emmc'
out.mkdir(parents=True, exist_ok=True)
driver = ROOT / 'drivers/XunEmmc'
source = driver / 'NativeEmmc.cpp'
obj = out / 'NativeEmmc.o'

command, i = [], 0
while i < len(cxx_recipe['arguments']):
    value = cxx_recipe['arguments'][i]
    if value == cxx_recipe['file']:
        command.append(str(source))
    elif value == '-o':
        command += ['-o', str(obj)]
        i += 1
    elif value in ('-MF', '-MJ'):
        i += 1
    elif value == '-Xclang' and cxx_recipe['arguments'][i + 1].startswith('-W'):
        i += 1
    elif not value.startswith('-W') and value not in ('-MD', '-MP'):
        command.append(value)
    i += 1

command += [
    '-I', str(ROOT / '.cache/xnu-work/osfmk'),
    '-I', str(ROOT / 'build/storage-abi/include'),
    '-I', str(driver),
    '-Wall', '-Wextra', '-Werror',
    '-Wno-unused-parameter', '-Wno-incompatible-sysroot', '-Wno-c99-designator',
    '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
    '-Wno-unnecessary-virtual-specifier',
]

result = subprocess.run(command, cwd=cxx_recipe['directory'], capture_output=True, text=True)
if result.returncode:
    sys.exit(str(source) + ':\n' + result.stderr[-2500:])

# Bind original IOBlockStorageDevice symbols if interface-audit.json exists
audit_file = ROOT / 'build/storage-abi/interface-audit.json'
bindings_obj = None
if audit_file.exists() and np:
    audit = json.loads(audit_file.read_text())
    obj_bytes = obj.read_bytes()
    image = np.macho(obj_bytes)
    offset = next(at for cmd, at, size in image['commands'] if cmd == 2)
    sy, count, st, _ = struct.unpack_from('<4I', obj_bytes, offset + 8)
    symbols = []
    for j in range(count):
        ix, kind, _, _, value = struct.unpack_from('<IBBHQ', obj_bytes, sy + j * 16)
        symbols.append((obj_bytes[st + ix:obj_bytes.index(b'\0', obj_bytes + ix)].decode(), value, kind))
    possible = {r['reference_name']: int(r['candidate_target'], 16) for r in audit['methods'] if not r['kernel_names']}
    possible.update({
        '__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': int(audit['original_protected_constructor'], 16),
        '__ZN20IOBlockStorageDeviceD2Ev': int(audit['original_base_destructor'], 16),
        '__ZN20IOBlockStorageDevice10gMetaClassE': int(audit['original_metaclass'], 16),
        '__ZTV20IOBlockStorageDevice': int(audit['vtable'], 16),
    })
    undefined = [name for name, value, kind in symbols if kind == 1 and ('IOBlockStorageDevice' in name)]
    bindings = {name: possible[name] for name in undefined if name in possible}
    if bindings:
        assembly = out / 'original-bindings.S'
        assembly.write_text(''.join(f'.globl {name}\n.set {name}, {value:#x}\n' for name, value in sorted(bindings.items())))
        bindings_obj = out / 'original-bindings.o'
        subprocess.run(['xcrun', 'clang', '-target', TRIPLE, '-Werror', '-Wno-incompatible-sysroot',
                        '-c', str(assembly), '-o', str(bindings_obj)], check=True)

archive = out / 'libXunEmmc.a'
archive.unlink(missing_ok=True)
ar_cmd = ['xcrun', 'ar', 'rcs', str(archive), str(obj)]
if bindings_obj and bindings_obj.exists():
    ar_cmd.append(str(bindings_obj))
subprocess.run(ar_cmd, check=True)
print('PASS: NativeEmmc archived to build/xun-emmc/libXunEmmc.a')
