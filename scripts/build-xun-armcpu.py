#!/usr/bin/env python3
"""Build UniXNU-xun's customized ArmCpu driver against the source-kernel ABI.

Uses upstream UniXNU ArmCpu with board-specific additions in package/ArmCpu.cpp
(configurable virtual timer PPI INTID, configurable SPI-IPI base, firmware
GICD_CTLR preservation when ARE_NS + EnableGrp1NS are already set, and boot
progress telemetry aliases), archiving build/armcpu/libArmCpu.a.
"""
import json
import os
from pathlib import Path
import subprocess

from kernel_target import TRIPLE, check_object


def main():
    root = Path(__file__).resolve().parents[1]
    out = root / 'build/armcpu'
    out.mkdir(parents=True, exist_ok=True)

    recipe = root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(recipe.read_text().strip().rstrip(','))

    candidates = [root / 'kits/xun/ArmCpu.cpp']
    if os.environ.get('UNIXNU_DEVICE'):
        candidates.append(Path(os.environ['UNIXNU_DEVICE']) / 'package/ArmCpu.cpp')
    candidates.append(root / 'package/ArmCpu.cpp')
    source = next((p for p in candidates if p.is_file()), None)
    if source is None:
        raise SystemExit('build-xun-armcpu: ArmCpu.cpp not found in ' + ', '.join(map(str, candidates)))

    obj = out / 'ArmCpu.o'
    arguments = record['arguments']
    command = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(obj)])
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1

    command.extend([
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-Wno-c99-designator', '-Wno-reorder-init-list',
        '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
        '-I', str(root / '.cache/xnu-work/osfmk'),
    ])
    (out / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (out / 'compile.log').write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout[-3500:])

    check_object(obj.read_bytes(), 'ArmCpu')
    archive = out / 'libArmCpu.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True)
    print(f'ArmCpu (xun): -Werror build and {TRIPLE} object audit passed -> {archive}')


if __name__ == '__main__':
    main()
