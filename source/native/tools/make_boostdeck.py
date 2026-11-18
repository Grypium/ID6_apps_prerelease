#!/usr/bin/env python3
"""BoostDeck with three rows of gauges and the Active Ethanol bar.

Writes plugin/BoostDeck.xml and plugin/BoostDeck_ALL_Images.zip:
  55000..55512  horizontal gauge bars (8 colors + 8 Multicolor, 32 steps; as make_bars.py)
  56100..56280  vertical Active Ethanol bars (9 palettes, 20 steps)
  56300         background: 12 titled tiles in a 4 x 3 grid + the ethanol column
Model and component IDs must match src/plugin.cpp (PERF_LAB build).
Run after make_bars.py (it overwrites the same two files with the older 2 x 4 layout).
"""
from io import BytesIO
from pathlib import Path
from xml.etree import ElementTree as ET
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from PIL import Image, ImageDraw, ImageFont

from make_bars import COLORS, MULTICOLOR, STEPS, FIRST, THEMES, add_props, button, label

ROOT = Path(__file__).resolve().parents[1]
# Tile order follows the model numbers: 0..7 as before (two per column), then the new row.
TITLES = ["BOOST", "AFR", "TIMING", "INTAKE AIR", "TRANSMISSION", "RAIL PRESSURE", "OIL PRESSURE", "COOLANT",
          "POWER (EST.)", "TORQUE", "THROTTLE", "OIL TEMP"]
BG_X, BG_Y, BG_W, BG_H = 24, 13, 1172, 400
TILE_W, TILE_H, COL_STEP, ROW_STEP = 240, 122, 258, 139
ETHANOL_STEPS, ETHANOL_FIRST, BACKGROUND = 20, 56100, 56300
ETHANOL_MULTI = '#80d6ff'
TRACK, TILE, TITLE = '#242b36', (23, 27, 34, 255), (190, 204, 219, 255)


def tile_at(i):
    """(column, row) of gauge i: 0..7 fill two rows column by column, 8..11 are row three."""
    return (i // 2, i % 2) if i < 8 else (i - 8, 2)


def font(size):
    for name in ("/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf", "DejaVuSansCondensed-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def png(image):
    data = BytesIO()
    image.save(data, format='PNG', optimize=True)
    return data.getvalue()


def background():
    image = Image.new('RGBA', (BG_W, BG_H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    title = font(21)
    for i, text in enumerate(TITLES):
        col, row = tile_at(i)
        x, y = col * COL_STEP, row * ROW_STEP
        draw.rounded_rectangle((x, y, x + TILE_W - 1, y + TILE_H - 1), radius=14, fill=TILE)
        draw.text((x + 16, y + 9), text, font=title, fill=TITLE)
    x = 4 * COL_STEP
    draw.rounded_rectangle((x, 0, BG_W - 1, BG_H - 1), radius=14, fill=TILE)
    draw.text((x + 18, 9), "ACTIVE E", font=title, fill=TITLE)
    return image


def ethanol_bars(archive):
    fills = [fill for _, fill in COLORS] + [ETHANOL_MULTI]
    for palette, fill in enumerate(fills):
        for step in range(ETHANOL_STEPS + 1):
            if palette and not step:
                continue
            image = Image.new('RGBA', (40, 250), (0, 0, 0, 0))
            draw = ImageDraw.Draw(image)
            draw.rounded_rectangle((0, 0, 39, 249), radius=8, fill=TRACK)
            if step:
                top = 250 - round(250 * step / ETHANOL_STEPS)
                draw.rounded_rectangle((0, top, 39, 249), radius=6, fill=fill)
            archive.writestr(ZipInfo(f'{ETHANOL_FIRST + palette * ETHANOL_STEPS + step}.png'), png(image), ZIP_DEFLATED)


def gauge_bars(archive):
    for color, fill in enumerate(tuple(fill for _, fill in COLORS) + MULTICOLOR):
        for step in range(STEPS + 1):
            if color and not step:
                continue
            image = Image.new('RGBA', (240, 12))
            draw = ImageDraw.Draw(image)
            draw.rounded_rectangle((0, 0, 239, 11), radius=4, fill='#263342')
            if step:
                draw.rounded_rectangle((0, 0, max(6, round(240 * step / STEPS)) - 1, 11), radius=4, fill=fill)
            archive.writestr(ZipInfo(f'{FIRST + color * STEPS + step}.png'), png(image), ZIP_DEFLATED)


def layout():
    root = ET.Element('pluginApps')
    app = ET.SubElement(root, 'pluginApp', applicationType='VehicleInformation', applicationWeight='210',
                        text='BoostDeck')
    entry = ET.SubElement(app, 'entryButton', id='1', action='10')
    ET.SubElement(ET.SubElement(entry, 'properties'), 'property', id='23', value='2')
    actions = ET.SubElement(app, 'actions')
    for identifier, target in ((10, 100), (31, 104)):
        ET.SubElement(actions, 'hmiAction', id=str(identifier), target=str(target))
    for identifier in list(range(40, 40 + len(THEMES))) + [49, 59, 120, 121, 122]:  # colors, units, max/live/reset
        ET.SubElement(actions, 'raAction', id=str(identifier))
    models = ET.SubElement(app, 'models')
    states = ET.SubElement(app, 'hmiStates')
    gauges = ET.SubElement(states, 'hmiState', id='100', text='BoostDeck')
    options = ET.SubElement(gauges, 'optionComponents')
    button(options, 33, 31, 'Gauge color')
    button(options, 410, 49, 'Units: Imperial')
    button(options, 411, 59, 'Units: Metric')
    button(options, 412, 120, 'Show: Max')
    button(options, 413, 121, 'Show: Live')
    button(options, 414, 122, 'Reset max')
    content = ET.SubElement(gauges, 'components')
    ET.SubElement(models, 'imageIdModel', id='69', imageId=str(BACKGROUND))
    add_props(ET.SubElement(content, 'image', id='209', model='69'), BG_X, BG_Y, BG_W, BG_H, True)
    for i in range(len(TITLES)):
        text_model, bar_model = (50 + i, 60 + i) if i < 8 else (70 + i - 8, 74 + i - 8)
        bar_id, label_id = (200 + i, 150 + i) if i < 8 else (210 + i - 8, 160 + i - 8)
        ET.SubElement(models, 'raDataModel', id=str(text_model), modelType='Richtext')
        ET.SubElement(models, 'imageIdModel', id=str(bar_model), imageId=str(FIRST))
        col, row = tile_at(i)
        x, y = BG_X + col * COL_STEP + 15, BG_Y + row * ROW_STEP
        add_props(ET.SubElement(content, 'image', id=str(bar_id), model=str(bar_model)), x, y + 97, 210, 15, True)
        add_props(label(content, label_id, text_model), x, y + 31, 216, 60)
    ET.SubElement(models, 'raDataModel', id='58', modelType='Richtext')
    ET.SubElement(models, 'imageIdModel', id='68', imageId=str(ETHANOL_FIRST))
    add_props(ET.SubElement(content, 'image', id='208', model='68'), 1106, 71, 40, 250, True)
    add_props(label(content, 158, 58), 1080, 335, 112, 63)
    ET.SubElement(models, 'raDataModel', id='110', modelType='Richtext')
    color_screen = ET.SubElement(states, 'hmiState', id='104', parentId='100', text='Gauge color')
    content = ET.SubElement(color_screen, 'components')
    label(content, 389, 110)
    for i, name in enumerate(THEMES):
        button(content, 390 + i, 40 + i, name)
    ET.indent(root, space='  ')
    return root


def main(out=ROOT / 'plugin'):
    out = Path(out)
    ET.ElementTree(layout()).write(out / 'BoostDeck.xml', encoding='utf-8', xml_declaration=True)
    with ZipFile(out / 'BoostDeck_ALL_Images.zip', 'w', compression=ZIP_DEFLATED) as archive:
        gauge_bars(archive)
        ethanol_bars(archive)
        archive.writestr(ZipInfo(f'{BACKGROUND}.png'), png(background()), ZIP_DEFLATED)


if __name__ == '__main__':
    import sys
    main(*sys.argv[1:])
