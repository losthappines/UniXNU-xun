#!/usr/bin/env python3
"""Parse, inspect, and validate Apple Device Tree (.adt) binaries.

Checks:
- Binary structure and 4-byte padding alignment of properties
- Root node 'device-tree' and standard children (/chosen, /cpus, /arm-io, etc.)
- CPU cores, cluster types, timebase frequency
- GICv3 interrupt controller and redistributor stride
- Storage (sdhci) and display (framebuffer) configurations
- Retained memory (ramoops)
"""
from pathlib import Path
import argparse
import struct
import sys


def parse_adt(data, offset=0, depth=0):
    if offset + 8 > len(data):
        raise ValueError(f'Truncated ADT node header at offset {offset}')
    num_props, num_children = struct.unpack_from('<II', data, offset)
    cursor = offset + 8
    properties = []
    for _ in range(num_props):
        if cursor + 36 > len(data):
            raise ValueError(f'Truncated ADT property header at offset {cursor}')
        raw_name, val_len = struct.unpack_from('<32sI', data, cursor)
        name = raw_name.split(b'\0')[0].decode('ascii', errors='replace')
        cursor += 36
        if cursor + val_len > len(data):
            raise ValueError(f'Truncated ADT property value for {name} at offset {cursor}')
        val_data = data[cursor:cursor + val_len]
        pad = (-val_len) % 4
        cursor += val_len + pad
        properties.append((name, val_data))

    children = []
    for _ in range(num_children):
        child_node, cursor = parse_adt(data, cursor, depth + 1)
        children.append(child_node)

    node_name = 'unnamed'
    for name, val in properties:
        if name == 'name':
            node_name = val.split(b'\0')[0].decode('ascii', errors='replace')
            break

    return {
        'name': node_name,
        'properties': properties,
        'children': children,
        'offset': offset,
        'size': cursor - offset
    }, cursor


def format_value(name, val):
    if len(val) == 0:
        return '<empty>'
    # String check
    if val.endswith(b'\0') and all(32 <= b <= 126 or b == 0 for b in val):
        parts = [p.decode('ascii') for p in val.split(b'\0') if p]
        return '"' + '", "'.join(parts) + '"'
    if len(val) == 4:
        u = struct.unpack('<I', val)[0]
        return f'{u} (0x{u:x})'
    if len(val) == 8:
        q = struct.unpack('<Q', val)[0]
        return f'{q} (0x{q:x})'
    if len(val) % 8 == 0 and len(val) <= 32:
        words = struct.unpack(f'<{len(val)//8}Q', val)
        return '[' + ', '.join(f'0x{w:x}' for w in words) + ']'
    return f'<{len(val)} bytes: {val[:16].hex()}...>'


def dump_node(node_dict, indent=0):
    sp = '  ' * indent
    print(f"{sp}+-- Node: '{node_dict['name']}' (props={len(node_dict['properties'])}, children={len(node_dict['children'])}, offset=0x{node_dict['offset']:x})")
    for name, val in node_dict['properties']:
        if name == 'name':
            continue
        print(f"{sp}    .{name} = {format_value(name, val)}")
    for child in node_dict['children']:
        dump_node(child, indent + 1)


def find_child(node_dict, name):
    for c in node_dict['children']:
        if c['name'] == name:
            return c
    return None


def get_prop(node_dict, name):
    for k, v in node_dict['properties']:
        if k == name:
            return v
    return None


def validate_xun_adt(root, is_smp=False):
    errors = []
    print("\n--- Validating xun ADT assertions ---")

    # Root
    if root['name'] != 'device-tree':
        errors.append(f"Root node name expected 'device-tree', got '{root['name']}'")

    # Chosen
    chosen = find_child(root, 'chosen')
    if not chosen:
        errors.append("Missing /chosen node")
    else:
        dram_base = get_prop(chosen, 'dram-base')
        if not dram_base or struct.unpack('<Q', dram_base)[0] not in (0x80000000, 0x40000000):
            errors.append(f"Invalid /chosen/dram-base: {dram_base.hex() if dram_base else 'missing'}")
        darwin_mem = get_prop(chosen, 'darwinboot-memory-size')
        if not darwin_mem or struct.unpack('<Q', darwin_mem)[0] != 0x80000000:
            errors.append(f"Invalid /chosen/darwinboot-memory-size: {darwin_mem.hex() if darwin_mem else 'missing'}")

    # CPUs
    cpus = find_child(root, 'cpus')
    if not cpus:
        errors.append("Missing /cpus node")
    else:
        num_cpus = len(cpus['children'])
        if is_smp and num_cpus != 8:
            errors.append(f"Expected 8 CPUs in SMP tree, found {num_cpus}")
        elif not is_smp and num_cpus != 1:
            errors.append(f"Expected 1 CPU in single tree, found {num_cpus}")
        for c in cpus['children']:
            tb = get_prop(c, 'timebase-frequency')
            if not tb or struct.unpack('<I', tb)[0] != 19200000:
                errors.append(f"{c['name']} timebase-frequency != 19200000 Hz")

    # PSCI
    if is_smp:
        psci = find_child(root, 'psci')
        if not psci:
            errors.append("Missing /psci node in SMP tree")

    # arm-io
    armio = find_child(root, 'arm-io')
    if not armio:
        errors.append("Missing /arm-io node")
    else:
        intc = find_child(armio, 'intc')
        if not intc:
            errors.append("Missing /arm-io/intc node")
        else:
            stride = get_prop(intc, 'redistributor-stride')
            if not stride or struct.unpack('<Q', stride)[0] != 0x20000:
                errors.append(f"Invalid GIC redistributor-stride: {stride.hex() if stride else 'missing'}")
        sdhci = find_child(armio, 'sdhci')
        if not sdhci:
            errors.append("Missing /arm-io/sdhci node")

    # Framebuffer
    fb = find_child(root, 'framebuffer')
    if not fb:
        errors.append("Missing /framebuffer node")
    else:
        w = get_prop(fb, 'width')
        h = get_prop(fb, 'height')
        reg = get_prop(fb, 'reg')
        if not w or struct.unpack('<I', w)[0] != 1200:
            errors.append(f"Invalid framebuffer width: {w.hex() if w else 'missing'}")
        if not h or struct.unpack('<I', h)[0] != 1920:
            errors.append(f"Invalid framebuffer height: {h.hex() if h else 'missing'}")
        if not reg or struct.unpack('<2Q', reg)[0] != 0x5C000000:
            errors.append(f"Invalid framebuffer base address: {reg.hex() if reg else 'missing'}")

    # Ramoops
    ramoops = find_child(root, 'ramoops')
    if not ramoops:
        errors.append("Missing /ramoops node")
    else:
        reg = get_prop(ramoops, 'reg')
        if not reg or struct.unpack('<2Q', reg)[0] != 0x5D000000:
            errors.append(f"Invalid ramoops base address: {reg.hex() if reg else 'missing'}")

    if errors:
        print("[FAIL] Validation failed with errors:")
        for e in errors:
            print(f"  * {e}")
        return False
    else:
        print("[PASS] All ADT structure and hardware constants verified successfully!")
        return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('adt_file', type=Path, help='Path to .adt binary')
    parser.add_argument('--smp', action='store_true', help='Validate as 8-core SMP tree')
    args = parser.parse_args()

    data = args.adt_file.read_bytes()
    print(f"Parsing ADT: {args.adt_file} ({len(data)} bytes)")
    tree, total = parse_adt(data)
    if total != len(data):
        print(f"WARNING: parsed {total} bytes but file size is {len(data)}")
    dump_node(tree)
    ok = validate_xun_adt(tree, is_smp=args.smp)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
