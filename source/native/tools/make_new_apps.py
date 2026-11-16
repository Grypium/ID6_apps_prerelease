#!/usr/bin/env python3
"""Write the FuelBlend and PerfTimers layouts, manifests and background art.

Usage: make_new_apps.py PLUGIN_DIR
Writes <App>.xml, <App>.manifest and <App>_ALL_Images.zip for both apps.
Model, action and component IDs here must match fuel_blend.cpp / perf_timers.cpp.
"""
import io
import sys
import zipfile
from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(sys.argv[1])
ROWS = [83, 150, 217, 284]


def props(x, y, w, h, extra=""):
    return (f'            <properties>\n              <property id="20" value="{x}" />\n'
            f'              <property id="21" value="{y}" />\n              <property id="9" value="{w}" />\n'
            f'              <property id="10" value="{h}" />\n{extra}            </properties>\n')


def image(cid):
    return (f'          <image id="{cid}" model="95">\n' +
            props(36, 9, 1120, 352, '              <property id="2" value="false" />\n'
                                    '              <property id="3" value="true" />\n') + '          </image>\n')


def label(cid, model, x, y, w, h=55):
    return f'          <label id="{cid}" model="{model}">\n' + props(x, y, w, h) + '          </label>\n'


def button(cid, action, text, x, y, w, h=55):
    return f'          <button id="{cid}" action="{action}" text="{text}">\n' + props(x, y, w, h) + '          </button>\n'


def app_xml(name, text, weight, image_id, actions, models, states):
    out = ["<?xml version='1.0' encoding='utf-8'?>\n<pluginApps>\n",
           f'  <pluginApp applicationType="VehicleInformation" applicationWeight="{weight}" text="{text}">\n',
           '    <entryButton id="1" action="10">\n      <properties>\n        <property id="23" value="2" />\n'
           '      </properties>\n    </entryButton>\n    <actions>\n', actions, '    </actions>\n    <models>\n']
    out += [f'      <raDataModel id="{m}" modelType="Richtext" />\n' for m in models]
    out += [f'      <imageIdModel id="95" imageId="{image_id}" />\n', '    </models>\n    <hmiStates>\n', states,
            '    </hmiStates>\n  </pluginApp>\n</pluginApps>\n']
    return "".join(out)


def manifest(name, connector):
    return f"""<?xml version='1.0' encoding='utf-8'?>
<plugin>
  <name>{name}</name>
  <type>App</type>
  <file>{{pluginDir}}/{name}</file>
  <config>
    <autoStart>yes</autoStart>
    <dependencies>
      <pluginAppConnector>
        <type>Etch</type>
        <id>{connector}</id>
        <asyncCallbacks>yes</asyncCallbacks>
      </pluginAppConnector>
    </dependencies>
    <parameters>
      <DescFile>{{pluginDir}}/{name}.xml</DescFile>
      <ImageDBFile>{{pluginDir}}/{name}_ALL_Images.zip</ImageDBFile>
    </parameters>
  </config>
</plugin>
"""


def art(path, image_id, divider_x=None, highlight_last=False):
    w, h = 1120, 352
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, w - 1, 62), radius=12, fill=(34, 44, 56, 255))
    for i in range(4):
        top = 70 + i * 67
        fill = (30, 40, 52, 255) if highlight_last and i == 3 else (23, 27, 34, 255)
        d.rounded_rectangle((0, top, w - 1, top + 59), radius=10, fill=fill)
        d.rounded_rectangle((8, top + 15, 12, top + 44), radius=2, fill=(96, 146, 196, 255))
        if divider_x:
            d.line((divider_x, top + 10, divider_x, top + 49), fill=(52, 58, 68, 255), width=2)
    png = io.BytesIO()
    img.save(png, "PNG", optimize=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        info = zipfile.ZipInfo(f"{image_id}.png", date_time=(1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.create_system = 3
        z.writestr(info, png.getvalue())


# --- FuelBlend ---------------------------------------------------------------
fb_actions = ('      <hmiAction id="10" target="100" />\n      <hmiAction id="40" target="101" />\n'
              '      <hmiAction id="41" target="102" />\n' +
              "".join(f'      <raAction id="{a}" />\n' for a in range(21, 39)))
main = ['      <hmiState id="100" text="Fuel Blend">\n        <optionComponents>\n'
        '          <button id="312" action="40" text="Pump settings" />\n'
        '          <button id="313" action="41" text="Calibrate E85" />\n        </optionComponents>\n'
        '        <components>\n', image(328), label(320, 80, 58, 20, 1067)]
for i, y in enumerate(ROWS[:3]):  # tank level, blend now, target: - / + buttons
    main += [label(321 + i, 81 + i, 62, y, 740), button(341 + 2 * i, 21 + 2 * i, "-", 820, y, 130),
             button(342 + 2 * i, 22 + 2 * i, "+", 970, y, 130)]
main += [label(324, 84, 62, 284, 1040),
         button(310, 27, "I filled up: save this blend", 49, 370, 520, 52),
         button(311, 40, "Pump settings", 589, 370, 520, 52),
         '        </components>\n      </hmiState>\n']
settings = ['      <hmiState id="101" parentId="100" text="Pump settings">\n        <components>\n', image(400),
            label(401, 88, 58, 20, 1067)]
for i, y in enumerate(ROWS[:2]):  # E85 content, pump gas ethanol
    settings += [label(402 + i, 86 + i, 62, y, 740), button(411 + 2 * i, 28 + 2 * i, "-", 820, y, 130),
                 button(412 + 2 * i, 29 + 2 * i, "+", 970, y, 130)]
settings += [button(415, 41, "Calibrate E85 from last fill", 49, 370, 520, 52),
             '        </components>\n      </hmiState>\n']
# Calibration: E85 pumped, 91 pumped, measured blend (- / +), result, apply.
calibrate = ['      <hmiState id="102" parentId="100" text="Calibrate E85">\n        <components>\n', image(500),
             label(501, 89, 58, 20, 1067)]
for i, y in enumerate(ROWS[:3]):
    calibrate += [label(502 + i, 90 + i, 62, y, 740), button(511 + 2 * i, 32 + 2 * i, "-", 820, y, 130),
                  button(512 + 2 * i, 33 + 2 * i, "+", 970, y, 130)]
calibrate += [label(505, 93, 62, 284, 1040), button(520, 38, "Use this for my E85", 49, 370, 520, 52),
              '        </components>\n      </hmiState>\n']
(OUT / "FuelBlend.xml").write_text(app_xml("FuelBlend", "Fuel Blend", 212, 56900, fb_actions,
                                            [80, 81, 82, 83, 84, 86, 87, 88, 89, 90, 91, 92, 93],
                                            "".join(main + settings + calibrate)))
(OUT / "FuelBlend.manifest").write_text(manifest("FuelBlend", 114))
art(OUT / "FuelBlend_ALL_Images.zip", 56900, highlight_last=True)

# --- PerfTimers --------------------------------------------------------------
pt_actions = '      <hmiAction id="10" target="100" />\n      <raAction id="26" />\n'
state = ['      <hmiState id="100" text="Perf Timers">\n        <optionComponents>\n'
         '          <button id="312" action="26" text="Reset best times" />\n        </optionComponents>\n'
         '        <components>\n', image(328), label(320, 80, 58, 20, 1067)]
for i, y in enumerate(ROWS):
    state += [label(321 + i, 82 + i, 62, y, 255), label(330 + i, 92 + i, 326, y, 801, 58)]
state += [label(335, 81, 58, 372, 1067), '        </components>\n      </hmiState>\n']
(OUT / "PerfTimers.xml").write_text(app_xml("PerfTimers", "Perf Timers", 213, 57000, pt_actions,
                                             [80, 81, 82, 83, 84, 85, 92, 93, 94, 95], "".join(state)))
(OUT / "PerfTimers.manifest").write_text(manifest("PerfTimers", 115))
art(OUT / "PerfTimers_ALL_Images.zip", 57000, divider_x=281)

# --- ExhaustFlap -------------------------------------------------------------
ef_actions = '      <hmiAction id="10" target="100" />\n' + "".join(f'      <raAction id="{a}" />\n' for a in (21, 22, 23))
state = ['      <hmiState id="100" text="Exhaust Flap">\n        <components>\n', image(328),
         label(320, 80, 58, 20, 1067), label(321, 81, 62, 83, 1040),
         button(341, 21, "Auto (car controls it)", 62, 150, 1040),
         button(342, 22, "Open", 62, 217, 1040),
         button(343, 23, "Closed", 62, 284, 1040),
         '        </components>\n      </hmiState>\n']
(OUT / "ExhaustFlap.xml").write_text(app_xml("ExhaustFlap", "Exhaust Flap", 214, 57100, ef_actions, [80, 81], "".join(state)))
(OUT / "ExhaustFlap.manifest").write_text(manifest("ExhaustFlap", 116))
art(OUT / "ExhaustFlap_ALL_Images.zip", 57100)
