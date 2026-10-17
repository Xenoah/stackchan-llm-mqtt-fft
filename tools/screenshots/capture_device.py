"""Render tagged device UI with real M5GFX rasterization/fonts and sample data.

Windows: needs Python + Pillow + ziglang, PlatformIO M5GFX dependencies and the
SDL2 2.32.10 MinGW development archive extracted under .pio/screenshots/sdl.
No firmware or connected hardware is changed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from PIL import Image
from capture_web import VERSIONS

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
CACHE = ROOT / '.pio/screenshots/native'
GFX = ROOT / '.pio/libdeps/m5stack-cores3/M5GFX'
SDL = ROOT / '.pio/screenshots/sdl/SDL2-2.32.10/x86_64-w64-mingw32'


def git_source(tag, path):
    return subprocess.check_output(['git', 'show', f'{tag}:{path}'], cwd=ROOT).decode('utf-8')


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def compile_one(source, destination, cpp, extra=()):
    command = [sys.executable, '-m', 'ziglang', 'c++' if cpp else 'cc', '-O1',
               '-DLGFX_SDL', *extra, '-I' + str(GFX / 'src'), '-I' + str(SDL / 'include'),
               '-c', str(source), '-o', str(destination)]
    if cpp:
        command[5:5] = ['-std=c++17', '-Wno-vla-cxx-extension']
    subprocess.run(command, cwd=ROOT, check=True)


def library():
    CACHE.mkdir(parents=True, exist_ok=True)
    # Recent M5GFX separates base, fonts and platform hubs into translation units.
    files = sorted((GFX / 'src/lgfx/v1').glob('*.cpp'))
    files += sorted((GFX / 'src/lgfx/Fonts').rglob('*.c'))
    files += sorted((GFX / 'src/lgfx/utility').glob('*.c'))
    objects = []
    for file in files:
        obj = CACHE / (file.stem + '.o')
        if not obj.exists() or obj.stat().st_mtime < file.stat().st_mtime:
            print('Compiling', file.name, flush=True)
            compile_one(file, obj, file.suffix == '.cpp')
        objects.append(obj)
    return objects


def capture(tag, objects):
    work = CACHE / tag
    work.mkdir(parents=True, exist_ok=True)
    out = ROOT / 'docs/screenshots' / tag
    out.mkdir(parents=True, exist_ok=True)
    sources = {}

    def source(path):
        text = git_source(tag, path)
        sources[path] = hashlib.sha256(text.encode()).hexdigest()
        return text

    for path in ['PrinterState.h', 'PrinterState.cpp', 'PrinterScreen.h', 'PrinterScreen.cpp']:
        (work / path).write_text(source('src/' + path), encoding='utf-8')
    main = source('src/main.cpp')
    depth = int(re.search(r'setColorDepth\((\d+)\)', function(main, 'M5Canvas& startupCanvas(')).group(1))
    has_keyboard = tag not in ['v2.0.0', 'v2.1.0']
    if has_keyboard:
        (work / 'SetupUi.h').write_text(source('src/SetupUi.h'), encoding='utf-8')
        (work / 'TouchKeyboard.h').write_text(source('src/TouchKeyboard.h'), encoding='utf-8')
        keyboard = source('src/TouchKeyboard.cpp')
        keyboard = keyboard[keyboard.index('using namespace setupui;'):keyboard.index('\nbool runTouchKeyboard(')]
        (work / 'keyboard_draw.inc').write_text(keyboard, encoding='utf-8')
    # Keep display functions unchanged; replace only the wall-clock ETA service.
    (work / 'PrinterJson.h').write_text('#pragma once\n#include "Arduino.h"\ninline String etaClockShort(int) { return "14:42"; }\n', encoding='utf-8')
    types = function(main, 'enum class AppMode') + ';\n' + function(main, 'enum class ModeMenuButton') + ';\n'
    (work / 'menu_types.inc').write_text(types, encoding='utf-8')
    constants = main[main.index('constexpr ModeMenuButton kMenuOrder'):main.index('bool modeMenuTileRect(')]
    draw = '\n'.join([function(main, 'const char* appModeName('), constants,
                      function(main, 'bool modeMenuTileRect('), function(main, 'void drawModeMenu(')])
    (work / 'menu_draw.inc').write_text(draw, encoding='utf-8')
    defines = ['-I' + str(HERE / 'native'), '-I' + str(work),
               f'-DCAPTURE_DEPTH={depth}', f'-DCAPTURE_KEYBOARD={int(has_keyboard)}',
               '-DCAPTURE_MODE=AppMode::' + ('LocalLlm' if tag == 'v2.0.0' else 'Printer'),
               '-DCAPTURE_AUTO=' + ('false' if tag == 'v2.0.0' else 'true')]
    app_objects = []
    for path in [work / 'PrinterState.cpp', work / 'PrinterScreen.cpp', HERE / 'native/render.cpp']:
        obj = work / (path.stem + '.o')
        compile_one(path, obj, True, defines)
        app_objects.append(obj)
    executable = work / 'render.exe'
    subprocess.run([sys.executable, '-m', 'ziglang', 'c++', '-O1', *map(str, objects + app_objects),
                    str(SDL / 'lib/libSDL2.dll.a'), '-o', str(executable)], check=True, cwd=ROOT)
    shutil.copy2(SDL / 'bin/SDL2.dll', work / 'SDL2.dll')
    subprocess.run([str(executable), str(work)], cwd=ROOT, check=True,
                   env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'})
    images = []
    for name in ['printer', 'menu'] + (['keyboard'] if has_keyboard else []):
        destination = out / f'device-{name}.png'
        with Image.open(work / f'device-{name}.ppm') as im:
            assert im.size == (320, 240)
            im.save(destination, optimize=True)
        images.append({'file': destination.name, 'width': 320, 'height': 240,
                       'sha256': hashlib.sha256(destination.read_bytes()).hexdigest()})
    (out / 'device-capture.json').write_text(json.dumps({
        'version': tag,
        'commit': subprocess.check_output(['git', 'rev-parse', f'{tag}^{{commit}}'], cwd=ROOT).decode().strip(),
        'capture': 'Device UI reproduction: tagged drawing functions executed on a PC; synthetic data, no device capture',
        'display': {'width': 320, 'height': 240, 'color_depth': depth},
        'renderer': {'library': 'M5GFX', 'version': json.loads((GFX / 'library.json').read_text())['version'],
                     'backend': 'SDL2 2.32.10, offscreen LGFX_Sprite',
                     'japanese_font': 'lgfxJapanGothicP_16 / lgfxJapanGothicP_12'},
        'sample': {'percent': 62, 'remaining_min': 42, 'eta': '14:42', 'layer': 155,
                   'total_layers': 250, 'wifi_ssid': 'StackChan-Demo', 'printer_ip': '192.0.2.25'},
        'source_sha256': sources, 'images': images,
    }, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'Rendered {tag}: {len(images)} device screens ({depth}bit)', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', choices=VERSIONS, action='append')
    args = parser.parse_args()
    if not (SDL / 'include/SDL2/SDL.h').exists():
        raise SystemExit('Extract SDL2-devel-2.32.10-mingw.zip into .pio/screenshots/sdl first')
    objects = library()
    for tag in args.version or VERSIONS:
        capture(tag, objects)


if __name__ == '__main__':
    main()
