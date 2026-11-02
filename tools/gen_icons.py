#!/usr/bin/env python3
"""Render the UI line icons to LVGL A8 images (recoloured at runtime) -> components/ui/icons.c / icons.h.

The shapes are drawn in the style of Tabler Icons (MIT). Needs rsvg-convert and ImageMagick's convert.
Usage: tools/gen_icons.py
"""
import os
import subprocess
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
OUT_C = os.path.join(ROOT, 'components/ui/icons.c')
OUT_H = os.path.join(ROOT, 'components/ui/icons.h')

# 24x24 line drawings
ICONS = {
    'wifi': '<path d="M12 18h.01"/><path d="M9.2 15.2a4 4 0 0 1 5.6 0"/><path d="M6.4 12.4a8 8 0 0 1 11.2 0"/>'
            '<path d="M3.5 9.5a12 12 0 0 1 17 0"/>',
    'wifi_off': '<path d="M12 18h.01"/><path d="M9.2 15.2a4 4 0 0 1 5.6 0"/><path d="M6.4 12.4a8 8 0 0 1 4.6 -2.3"/>'
                '<path d="M3.5 9.5a12 12 0 0 1 4.3 -2.7"/><path d="M3 3l18 18"/>',
    'settings': '<path d="M10.3 4.3c.4-1.8 2.9-1.8 3.4 0a1.7 1.7 0 0 0 2.6 1.1c1.5-.9 3.3.8 2.4 2.4a1.7 1.7 0 0 0 1 2.5'
                'c1.8.4 1.8 2.9 0 3.4a1.7 1.7 0 0 0-1 2.6c.9 1.5-.9 3.3-2.4 2.4a1.7 1.7 0 0 0-2.6 1c-.4 1.8-2.9 1.8-3.4 0'
                'a1.7 1.7 0 0 0-2.5-1c-1.6.9-3.3-.9-2.4-2.4a1.7 1.7 0 0 0-1.1-2.6c-1.8-.4-1.8-2.9 0-3.4a1.7 1.7 0 0 0 1.1-2.5'
                'c-.9-1.6.8-3.3 2.4-2.4 1 .6 2.3.1 2.5-1.1z"/><path d="M9 12a3 3 0 1 0 6 0a3 3 0 0 0 -6 0"/>',
    'droplet': '<path d="M7.5 7.5l4.5-4.5l4.5 4.5a6.4 6.4 0 1 1 -9 0z"/>',
    'fan': '<path d="M12 12c-1.5-3.5-.5-7 3-8c1.5 3 .5 6.5-3 8"/><path d="M12 12c3.5 1.5 7 .5 8-3c-3-1.5-6.5-.5-8 3"/>'
           '<path d="M12 12c1.5 3.5 .5 7-3 8c-1.5-3-.5-6.5 3-8"/><path d="M12 12c-3.5-1.5-7-.5-8 3c3 1.5 6.5 .5 8-3"/>',
    'exchange': '<path d="M3 9h16l-3-3"/><path d="M21 15H5l3 3"/>',
    'holiday': '<path d="M3 12a9 9 0 0 1 18 0z"/><path d="M12 12v6a2 2 0 0 0 4 0"/><path d="M12 3v1"/>',
    'moon': '<path d="M12 3a6 6 0 0 0 9 9a9 9 0 1 1 -9 -9z"/>',
    'hourglass': '<path d="M6.5 7h11"/><path d="M6.5 17h11"/><path d="M6 20v-2a6 6 0 1 1 12 0v2a1 1 0 0 1 -1 1h-10a1 1 0 0 1 -1 -1z"/>'
                 '<path d="M6 4v2a6 6 0 1 0 12 0v-2a1 1 0 0 0 -1 -1h-10a1 1 0 0 0 -1 1z"/>',
    'clock': '<path d="M3 12a9 9 0 1 0 18 0a9 9 0 0 0 -18 0"/><path d="M12 7v5l3 3"/>',
    'filter': '<path d="M4 4h16v2.2a2 2 0 0 1 -.6 1.4l-4.4 4.4v7l-6 2v-8.5l-4.5-4.9a2 2 0 0 1 -.5-1.3z"/>',
    'device': '<path d="M3 5a1 1 0 0 1 1 -1h16a1 1 0 0 1 1 1v10a1 1 0 0 1 -1 1h-16a1 1 0 0 1 -1 -1z"/><path d="M7 20h10"/>'
              '<path d="M9 16v4"/><path d="M15 16v4"/>',
    'screen_off': '<path d="M3 5a1 1 0 0 1 1 -1h16a1 1 0 0 1 1 1v10a1 1 0 0 1 -1 1h-16a1 1 0 0 1 -1 -1z"/><path d="M7 20h10"/>'
                  '<path d="M3 3l18 18"/>',
    'tool': '<path d="M7 10h3v-3l-3.5-3.5a6 6 0 0 1 8 8l6 6a2 2 0 0 1 -3 3l-6-6a6 6 0 0 1 -8-8l3.5 3.5"/>',
    'alert': '<path d="M12 9v4"/><path d="M10.4 3.9l-8 13.6a1.9 1.9 0 0 0 1.6 2.9h16a1.9 1.9 0 0 0 1.6-2.9l-8-13.6'
             'a1.9 1.9 0 0 0-3.2 0z"/><path d="M12 16h.01"/>',
    'failure': '<path d="M3 12a9 9 0 1 0 18 0a9 9 0 0 0 -18 0"/><path d="M10 10l4 4m0-4l-4 4"/>',
    'bell': '<path d="M10 5a2 2 0 1 1 4 0a7 7 0 0 1 4 6v3a4 4 0 0 0 2 3h-16a4 4 0 0 0 2 -3v-3a7 7 0 0 1 4 -6"/>'
            '<path d="M9 17v1a3 3 0 0 0 6 0v-1"/><path d="M21 6.7a10 10 0 0 0 -2.6-3.7"/><path d="M3 6.7a10 10 0 0 1 2.6-3.7"/>',
    'flame': '<path d="M12 12c2-2.96 0-7-1-8c0 3.04-1.77 4.74-3 6c-1.22 1.27-2 3.27-2 5a6 6 0 1 0 12 0'
             'c0-1.5-1.06-3.86-2-5c-1.79 3-2.79 3-4 2z"/>',
    'wind': '<path d="M5 8h8.5a2.5 2.5 0 1 0 -2.34 -3.24"/><path d="M3 12h15.5a2.5 2.5 0 1 1 -2.34 3.24"/>'
            '<path d="M4 16h5.5a2.5 2.5 0 1 1 -2.34 3.24"/>',
    'info': '<path d="M3 12a9 9 0 1 0 18 0a9 9 0 0 0 -18 0"/><path d="M12 9h.01"/><path d="M11 12h1v4h1"/>',
    'sun': '<path d="M8 12a4 4 0 1 0 8 0a4 4 0 1 0 -8 0"/><path d="M3 12h1"/><path d="M12 3v1"/><path d="M20 12h1"/>'
           '<path d="M12 20v1"/><path d="M5.6 5.6l.7 .7"/><path d="M18.4 5.6l-.7 .7"/><path d="M17.7 17.7l.7 .7"/>'
           '<path d="M6.3 17.7l-.7 .7"/>',
    'home': '<path d="M5 12l-2 0l9 -9l9 9l-2 0"/><path d="M5 12v7a2 2 0 0 0 2 2h10a2 2 0 0 0 2 -2v-7"/>',
}

# (icon, pixel size) pairs the UI uses
SIZES = [
    ('wifi', 18), ('wifi_off', 18), ('settings', 22), ('droplet', 14), ('fan', 26), ('exchange', 24),
    ('holiday', 28), ('moon', 28), ('droplet', 28), ('hourglass', 28), ('exchange', 28), ('clock', 28),
    ('filter', 28), ('device', 28),
    ('tool', 32), ('alert', 32), ('failure', 32), ('alert', 16), ('bell', 64),
    ('flame', 24), ('wind', 24), ('holiday', 24), ('home', 24), ('home', 18), ('home', 20), ('sun', 20),
    ('wifi', 20), ('moon', 20), ('screen_off', 20), ('info', 20), ('clock', 20), ('hourglass', 20),
]


def render(name, size):
    stroke = 1.7 if size >= 48 else max(2.0, 1.6 * 24 / size)
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#000" '
           f'stroke-width="{stroke:.2f}" stroke-linecap="round" stroke-linejoin="round">{ICONS[name]}</svg>')
    with tempfile.TemporaryDirectory() as tmp:
        src, png = os.path.join(tmp, 'i.svg'), os.path.join(tmp, 'i.png')
        open(src, 'w').write(svg)
        subprocess.run(['rsvg-convert', '-w', str(size), '-h', str(size), '-o', png, src], check=True)
        alpha = subprocess.run(['convert', png, '-alpha', 'extract', '-depth', '8', 'gray:-'], check=True,
                               capture_output=True).stdout
    assert len(alpha) == size * size, (name, size, len(alpha))
    return alpha


def main():
    c = ['/* Generated by tools/gen_icons.py, do not edit. Line icons in the style of Tabler Icons (MIT). */',
         '#include "icons.h"', '']
    h = ['#pragma once', '/* Generated by tools/gen_icons.py. A8 images: set image_recolor to colour them. */',
         '#include "lvgl.h"', '']
    for name, size in SIZES:
        sym = f'ic_{name}_{size}'
        data = render(name, size)
        c.append(f'static const uint8_t {sym}_map[] = {{')
        for i in range(0, len(data), 24):
            c.append('    ' + ', '.join(f'0x{b:02x}' for b in data[i:i + 24]) + ',')
        c.append('};')
        c.append(f'const lv_image_dsc_t {sym} = {{')
        c.append(f'    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = {size}, .h = {size}, '
                 f'.stride = {size}}},')
        c.append(f'    .data_size = sizeof({sym}_map),')
        c.append(f'    .data = {sym}_map,')
        c.append('};')
        c.append('')
        h.append(f'extern const lv_image_dsc_t {sym};')
    open(OUT_C, 'w').write('\n'.join(c) + '\n')
    open(OUT_H, 'w').write('\n'.join(h) + '\n')
    print(f'{len(SIZES)} icons -> {os.path.relpath(OUT_C, ROOT)}')


if __name__ == '__main__':
    main()
