from pathlib import Path
import struct, zlib

def chunk(name, data):
    return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))

def png16(path, width, height, color, rows):
    data = b''.join(b'\x00' + struct.pack('>' + 'H' * len(row), *row) for row in rows)
    header = struct.pack('>IIBBBBB', width, height, 16, color, 0, 0, 0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
                     chunk(b'IDAT', zlib.compress(data)) + chunk(b'IEND', b''))

out = Path('build/fixtures')
out.mkdir(parents=True, exist_ok=True)
png16(out / 'gray16.png', 2, 2, 0, [[30000, 30001], [30002, 30003]])
png16(out / 'rgb16.png', 2, 2, 2, [[30000, 30001, 30002, 30003, 30004, 30005], [0, 32768, 65535, 1, 2, 3]])

def tiff16(path, width, height, samples, values):
    entries = [
        (256, 4, 1, width), (257, 4, 1, height), (258, 3, samples, 16),
        (259, 3, 1, 1), (262, 3, 1, 1 if samples == 1 else 2),
        (273, 4, 1, 0), (277, 3, 1, samples), (278, 4, 1, height),
        (279, 4, 1, len(values) * 2), (284, 3, 1, 1),
    ]
    extra = b'' if samples == 1 else struct.pack('<HHH', 16, 16, 16)
    ifd_end = 8 + 2 + len(entries) * 12 + 4
    entries[5] = (273, 4, 1, ifd_end + len(extra))
    if extra:
        entries[2] = (258, 3, samples, ifd_end)
    tags = b''.join(struct.pack('<HHII', *entry) for entry in entries)
    data = struct.pack('<' + 'H' * len(values), *values)
    path.write_bytes(b'II' + struct.pack('<HIH', 42, 8, len(entries)) + tags + b'\0' * 4 + extra + data)

tiff16(out / 'gray16.tif', 2, 2, 1, [30000, 30001, 30002, 30003])
tiff16(out / 'rgb16.tif', 2, 2, 3, [30000, 30001, 30002, 30003, 30004, 30005, 0, 32768, 65535, 1, 2, 3])
