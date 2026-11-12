#!/usr/bin/env python3
"""XunEmmc (drivers/XunEmmc/NativeEmmc.cpp): build/xun-emmc/libXunEmmc.a,
the archive that CONFIG_XNU_NATIVE_EMMC links (CONFIG_XNU_NATIVE_EMMC_ARCHIVE)."""
import json, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
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
    '-I', str(ROOT / '.cache/xnu-work/osfmks'),
    '-I', str(driver),
    '-Wall', '-Wextra', '-Werror',
    '-Wno-unused-parameter', '-Wno-incompatible-sysroot', '-Wno-c99-designator',
    '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
    '-Wno-unnecessary-virtual-specifier',
]

result = subprocess.run(command, cwd=cxx_recipe['directory'], capture_output=True, text=True)
if result.returncode:
    sys.exit(str(source) + ':\n' + result.stderr[-2500:])

archive = out / 'libXunEmmc.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, obj], check=True)
print('PASS: NativeEmmc archived to build/xun-emmc/libXunEmmc.a')
