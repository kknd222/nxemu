"""Change the application identity of a verified APK without changing its code or resources."""
import hashlib
import struct
import sys
import zipfile


def length8(data, offset):
    value = data[offset]
    return (((value & 127) << 8) | data[offset + 1], offset + 2) if value & 128 else (value, offset + 1)


def length16(data, offset):
    value = struct.unpack_from('<H', data, offset)[0]
    return (((value & 32767) << 16) | struct.unpack_from('<H', data, offset + 2)[0], offset + 4) if value & 32768 else (value, offset + 2)


def encode8(length):
    if length >= 32768:
        raise ValueError('String exceeds binary XML limit')
    return bytes([(length >> 8) | 128, length & 255]) if length >= 128 else bytes([length])


def encode16(length):
    return struct.pack('<HH', (length >> 16) | 32768, length & 65535) if length >= 32768 else struct.pack('<H', length)


def patch_pool(pool):
    kind, header, size, count, style_count, flags, start, styles_start = struct.unpack_from('<HH6I', pool)
    assert kind == 1 and size == len(pool)
    offsets = struct.unpack_from('<' + 'I' * count, pool, header)
    strings, changed = [], set()
    replacements = {'org.nxemu': 'org.nxemu.work', 'NxEmu': 'NxEmu Work'}
    for offset in offsets:
        pos = start + offset
        if flags & 256:
            _, pos = length8(pool, pos)
            length, pos = length8(pool, pos)
            value = pool[pos:pos + length].decode('utf-8')
        else:
            length, pos = length16(pool, pos)
            value = pool[pos:pos + length * 2].decode('utf-16-le')
        if value in replacements:
            changed.add(value)
            value = replacements[value]
        utf16 = value.encode('utf-16-le')
        if flags & 256:
            utf8 = value.encode('utf-8')
            strings.append(encode8(len(utf16) // 2) + encode8(len(utf8)) + utf8 + b'\0')
        else:
            strings.append(encode16(len(utf16) // 2) + utf16 + b'\0\0')
    assert changed == set(replacements), changed
    data, offsets = bytearray(), []
    for value in strings:
        offsets.append(len(data))
        data.extend(value)
    data.extend(b'\0' * (-len(data) % 4))
    styles = pool[styles_start:] if styles_start else b''
    prefix = bytearray(pool[:start])
    struct.pack_into('<' + 'I' * count, prefix, header, *offsets)
    struct.pack_into('<I', prefix, 16, flags & ~1)  # String order may no longer be sorted.
    struct.pack_into('<I', prefix, 24, start + len(data) if styles_start else 0)
    result = prefix + data + styles
    struct.pack_into('<I', result, 4, len(result))
    return result


def patch_manifest(xml):
    kind, header, size = struct.unpack_from('<HHI', xml)
    assert kind == 3 and size == len(xml)
    result = bytearray(xml[:header])
    offset, count = header, 0
    while offset < len(xml):
        kind, _, size = struct.unpack_from('<HHI', xml, offset)
        assert size >= 8 and offset + size <= len(xml)
        chunk = xml[offset:offset + size]
        if kind == 1:
            chunk = patch_pool(chunk)
            count += 1
        result.extend(chunk)
        offset += size
    assert count == 1
    struct.pack_into('<I', result, 4, len(result))
    return result


def signature(name):
    name = name.upper()
    return name == 'META-INF/MANIFEST.MF' or (name.startswith('META-INF/') and name.endswith(('.SF', '.RSA', '.DSA', '.EC')))


if __name__ == '__main__':
    source, target = sys.argv[1:]
    with zipfile.ZipFile(source) as original, zipfile.ZipFile(target, 'w') as output:
        for item in original.infolist():
            if signature(item.filename):
                continue
            data = original.read(item.filename)
            if item.filename == 'AndroidManifest.xml':
                data = patch_manifest(data)
            output.writestr(item, data)
    with zipfile.ZipFile(source) as original, zipfile.ZipFile(target) as patched:
        for item in original.infolist():
            if item.filename != 'AndroidManifest.xml' and not signature(item.filename):
                assert hashlib.sha256(original.read(item.filename)).digest() == hashlib.sha256(patched.read(item.filename)).digest(), item.filename
    print('Verified: only application manifest/signatures changed; all code and resources are byte-identical')
