#!/usr/bin/env python3
"""Replace one PNG icon's bitmap in src/gui/shell/icons.c.

The repo's own tools/svg_to_lvgl.py needs cairosvg + pillow, which this host
does not have. For swapping a *raster* asset there is nothing to rasterise --
the source is already a PNG -- so this reads it with the standard library only
and rewrites the single `icon_<name>_data[]` array in place.

The generated format is the one svg_to_lvgl.py emits for PNGs: ARGB8888,
rows of 4-byte pixels, stride = w * 4, no padding.

    python3 tools/png_icon_swap.py <png> <c_identifier> <size> [--silhouette]

`--silhouette` is for artwork drawn black-on-white: it becomes a white shape on
a transparent field, which is what the menu tiles use.

Everything else in icons.c is left byte-for-byte alone, so the diff is one
array and nothing else.
"""
import re
import struct
import sys
import zlib


def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit('%s is not a PNG' % path)
    i = 8
    idat = b''
    plte = trns = None
    w = h = bit_depth = color_type = interlace = None
    while i < len(data):
        length = struct.unpack('>I', data[i:i + 4])[0]
        kind = data[i + 4:i + 8]
        chunk = data[i + 8:i + 8 + length]
        i += 12 + length
        if kind == b'IHDR':
            w, h, bit_depth, color_type, _comp, _filt, interlace = struct.unpack('>IIBBBBB', chunk)
        elif kind == b'IDAT':
            idat += chunk
        elif kind == b'PLTE':
            plte = chunk
        elif kind == b'tRNS':
            trns = chunk
        elif kind == b'IEND':
            break
    if bit_depth != 8 or interlace != 0:
        raise SystemExit('only 8-bit non-interlaced PNGs are handled (got depth %d, interlace %d)'
                         % (bit_depth, interlace))
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(idat)
    stride = w * channels
    out = bytearray()
    prev = bytearray(stride)
    pos = 0
    for _y in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if f == 1:
            for x in range(channels, stride):
                line[x] = (line[x] + line[x - channels]) & 255
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x - channels] if x >= channels else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - channels] if x >= channels else 0
                b = prev[x]
                c = prev[x - channels] if x >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 255
        elif f != 0:
            raise SystemExit('bad PNG filter %d' % f)
        out += line
        prev = line
    return w, h, color_type, plte, trns, bytes(out)


def pixels(path):
    w, h, color_type, plte, trns, raw = read_png(path)
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]

    def at(x, y):
        o = (y * w + x) * channels
        if color_type == 3:
            idx = raw[o]
            r, g, b = plte[idx * 3:idx * 3 + 3]
            a = trns[idx] if trns and idx < len(trns) else 255
            return r, g, b, a
        if color_type == 0:
            v = raw[o]
            return v, v, v, 255
        if color_type == 4:
            v, a = raw[o], raw[o + 1]
            return v, v, v, a
        if color_type == 2:
            return raw[o], raw[o + 1], raw[o + 2], 255
        return raw[o], raw[o + 1], raw[o + 2], raw[o + 3]

    return w, h, at


def resample(path, size, white_silhouette=False):
    """Box-filter down to `size`, keeping colour and alpha.

    `white_silhouette` turns a black-on-white drawing into the form every other
    menu tile uses: a white shape on a transparent field. The ink becomes the
    alpha, so the tile can still be recoloured at draw time, and an opaque
    background does not put a pale square on a dark theme.
    """
    w, h, at = pixels(path)
    scale = w / size
    rows = []
    for y in range(size):
        row = []
        y0, y1 = int(y * scale), max(int(y * scale) + 1, int((y + 1) * scale))
        for x in range(size):
            x0, x1 = int(x * scale), max(int(x * scale) + 1, int((x + 1) * scale))
            r = g = b = a = 0
            n = 0
            for yy in range(y0, min(y1, h)):
                for xx in range(x0, min(x1, w)):
                    pr, pg, pb, pa = at(xx, yy)
                    if white_silhouette:
                        # Ink is darkness: a white background becomes nothing.
                        ink = (255 - (pr + pg + pb) // 3) * pa // 255
                        pr = pg = pb = 255
                        pa = ink
                    # Premultiply so transparent pixels do not darken the edge.
                    r += pr * pa
                    g += pg * pa
                    b += pb * pa
                    a += pa
                    n += 1
            if a:
                row.append((r // a, g // a, b // a, a // n))
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    return rows


def emit(rows, size, identifier):
    body = []
    for row in rows:
        line = '\t'
        for (r, g, b, a) in row:
            line += '0x%02x, 0x%02x, 0x%02x, 0x%02x, ' % (b, g, r, a)  # ARGB8888 in memory order
        body.append(line.rstrip(' ,') + ',')
    text = 'static const uint8_t %s_data[] = {\n%s\n};\n' % (identifier, '\n'.join(body))

    header = ('const lv_image_dsc_t %s = {\n'
              '\t.header = {\n'
              '\t\t.magic = LV_IMAGE_HEADER_MAGIC,\n'
              '\t\t.cf = LV_COLOR_FORMAT_ARGB8888,\n'
              '\t\t.w = %d,\n'
              '\t\t.h = %d,\n'
              '\t\t.stride = %d,\n'
              '\t},\n'
              '\t.data_size = sizeof(%s_data),\n'
              '\t.data = %s_data,\n'
              '};\n') % (identifier, size, size, size * 4, identifier, identifier)
    return text, header


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 3:
        raise SystemExit(__doc__)
    png, identifier, size = args[0], args[1], int(args[2])
    rows = resample(png, size, white_silhouette='--silhouette' in sys.argv)
    array_text, header_text = emit(rows, size, identifier)

    out_c = 'src/gui/shell/icons.c'
    source = open(out_c, encoding='utf-8').read()

    # The comment above the array names the source file, and is what tells a
    # reader which asset this block came from -- update it with the array.
    array_re = re.compile(r'(static const uint8_t %s_data\[\] = \{\n).*?(\n\};\n)' % re.escape(identifier),
                          re.S)
    if not array_re.search(source):
        raise SystemExit('no %s_data[] in %s' % (identifier, out_c))
    source = array_re.sub(lambda m: m.group(1) + array_text.split('\n', 1)[1].rsplit('\n};', 1)[0] + m.group(2),
                          source, count=1)

    dsc_re = re.compile(r'const lv_image_dsc_t %s = \{.*?\n\};\n' % re.escape(identifier), re.S)
    if not dsc_re.search(source):
        raise SystemExit('no %s descriptor in %s' % (identifier, out_c))
    source = dsc_re.sub(lambda m: header_text, source, count=1)

    open(out_c, 'w', encoding='utf-8').write(source)
    print('rewrote %s (%dx%d) in %s' % (identifier, size, size, out_c))


if __name__ == '__main__':
    main()
