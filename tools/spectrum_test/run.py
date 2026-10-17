"""Offline DSP checks; --ui also checks/renders the real menu, face and FFT UI.

Requires Python ziglang. --ui additionally needs Pillow and the SDL2 setup
from tools/screenshots/capture_device.py. Never opens a serial port.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / '.pio/spectrum-test'

def run(command, **kwargs):
    subprocess.run(list(map(str, command)), cwd=ROOT, check=True, **kwargs)

def usb():
    sys.path.insert(0, str(ROOT / 'tools/screenshots'))
    from capture_device import function
    work=WORK/'usb';work.mkdir(exist_ok=True)
    sdk=Path.home()/'.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/include/arduino_tinyusb/tinyusb/src'
    source=(ROOT/'src/usb/UsbAudio.cpp').read_text(encoding='utf-8')
    handlers='\n'.join(function(source,name) for name in ['uint16_t loadDescriptor(',
        'bool tud_audio_get_req_entity_cb(', 'bool tud_audio_set_req_entity_cb('])
    (work/'usb_handlers.inc').write_text(handlers,encoding='utf-8')
    config='#pragma once\n#define CFG_TUSB_MCU OPT_MCU_NONE\n#define CFG_TUSB_OS OPT_OS_NONE\n'
    config+='#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE\n#define CFG_TUD_ENDPOINT0_SIZE 64\n'
    config+='#include "UsbAudioConfig.h"\n'
    (work/'tusb_config.h').write_text(config)
    exe=work/'usb.exe'
    run([sys.executable,'-m','ziglang','c++','-std=c++17','-O1','-I'+str(work),
        '-I'+str(sdk),'-I'+str(ROOT/'src/usb'),ROOT/'tools/spectrum_test/usb.cpp','-o',exe])
    run([exe])

def ui():
    sys.path.insert(0, str(ROOT / 'tools/screenshots'))
    import capture_device as cap
    from PIL import Image
    if not (cap.SDL / 'include/SDL2/SDL.h').exists():
        raise SystemExit('Install SDL2 as described in tools/screenshots/capture_device.py')
    work = WORK / 'ui'
    work.mkdir(parents=True, exist_ok=True)
    arduino = (cap.HERE / 'native/Arduino.h').read_text(encoding='utf-8')
    arduino = arduino.replace('inline uint32_t millis() { return 10000; }',
        'inline uint32_t captureNow=10000; inline uint32_t millis() { return captureNow; }')
    arduino += '\nusing portMUX_TYPE=int;\n#define portMUX_INITIALIZER_UNLOCKED 0\n'
    arduino += '#define portENTER_CRITICAL(x) ((void)(x))\n#define portEXIT_CRITICAL(x) ((void)(x))\n'
    arduino += '#define M5_LOGI(...) ((void)0)\ntemplate<class T> T constrain(T n,T lo,T hi) {return std::clamp(n,lo,hi);}\n'
    (work / 'Arduino.h').write_text(arduino,encoding='utf-8')
    m5 = (cap.HERE / 'native/M5Unified.h').read_text(encoding='utf-8')
    m5 = m5.replace(' public:', ''' public:
  M5Canvas(M5Canvas* = nullptr) {}
  void waitDMA() {}
  void pushImageDMA(int x,int y,int w,int h,const lgfx::swap565_t* data) { pushImage(x,y,w,h,data); }
''')
    (work / 'M5Unified.h').write_text(m5,encoding='utf-8')
    (work / 'M5GFX.h').write_text('#pragma once\n#include "M5Unified.h"\n')
    (work / 'esp_timer.h').write_text('#pragma once\n#include "Arduino.h"\ninline int64_t esp_timer_get_time() {return int64_t(captureNow)*1000;}\n')
    (work / 'esp_heap_caps.h').write_text('''#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
inline void* heap_caps_malloc(size_t n,int) {return malloc(n);}
inline void* heap_caps_calloc(size_t n,size_t s,int) {return calloc(n,s);}
inline void heap_caps_free(void* p) {free(p);}
''')
    main = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
    (work / 'menu_types.inc').write_text(cap.function(main,'enum class AppMode')+';\n'+cap.function(main,'enum class ModeMenuButton')+';\n',encoding='utf-8')
    constants = main[main.index('constexpr ModeMenuButton kMenuOrder'):main.index('bool modeMenuTileRect(')]
    draw = '\n'.join([cap.function(main,'const char* appModeName('),constants,
        cap.function(main,'bool modeMenuTileRect('),cap.function(main,'ModeMenuButton modeMenuButtonAt('),
        cap.function(main,'void drawModeMenu(')])
    (work / 'menu_draw.inc').write_text(draw,encoding='utf-8')
    avatar = ROOT / '.pio/libdeps/m5stack-cores3/M5Stack-Avatar/src'
    adapter = work / 'avatar'; adapter.mkdir(exist_ok=True)
    names = ['BoundingRect.cpp','ColorPalette.cpp','DrawContext.cpp','Gaze.cpp','Eye.cpp','Mouth.cpp','Eyeblow.cpp']
    for path in [*avatar.glob('*.h'),*(avatar/n for n in names)]:
        text = path.read_text(encoding='utf-8')
        if path.name == 'DrawContext.h':
            text = text.replace('#ifndef ARDUINO\n#include <string>\ntypedef std::string String;\n#endif  // ARDUINO','#include "Arduino.h"')
        (adapter/path.name).write_text(text,encoding='utf-8')
    face = (avatar/'Face.cpp').read_text(encoding='utf-8')
    constructor = face[face.index('Face::Face()'):face.index('Face::Face(Drawable')]
    parts = re.findall(r'new (Mouth|Eye|Eyeblow)\(([^)]+)\),\s*new BoundingRect\(([^)]+)\)',constructor)
    assert len(parts)==5
    drawparts=[]
    for i,(kind,params,rect) in enumerate(parts):
        create=f'HudMouth part{i}(new Mouth({params}),&hud);' if kind=='Mouth' else f'{kind} part{i}({params});'
        drawparts.append(create+f' part{i}.draw(&c,BoundingRect({rect}),&ctx);')
    source = (ROOT/'tools/spectrum_test/ui.cpp').read_text().replace('@PARTS@','\n'.join(drawparts))
    (work/'ui.cpp').write_text(source,encoding='utf-8')
    objects = cap.library()
    defines = ['-I'+str(work),'-I'+str(adapter),'-I'+str(ROOT/'src')]
    for path in [*(adapter/n for n in names),ROOT/'src/FaceHud.cpp',ROOT/'src/PrinterState.cpp',work/'ui.cpp']:
        obj = work/(path.stem+'.o'); cap.compile_one(path,obj,True,defines); objects.append(obj)
    exe=work/'ui.exe'
    run([sys.executable,'-m','ziglang','c++','-O1',*objects,cap.SDL/'lib/libSDL2.dll.a','-o',exe])
    shutil.copy2(cap.SDL/'bin/SDL2.dll',work/'SDL2.dll')
    run([exe,work],env={**os.environ,'SDL_VIDEODRIVER':'dummy'})
    for ppm in work.glob('*.ppm'):
        with Image.open(ppm) as im:
            im.save(ppm.with_suffix('.png'))
    print('UI previews (synthetic audio, no hardware):', work)

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ui',action='store_true')
    args=parser.parse_args()
    WORK.mkdir(parents=True,exist_ok=True)
    os.environ['ZIG_GLOBAL_CACHE_DIR']=str(WORK/'zig-global')
    os.environ['ZIG_LOCAL_CACHE_DIR']=str(WORK/'zig-local')
    exe=WORK/'dsp.exe'
    run([sys.executable,'-m','ziglang','c++','-std=c++17','-O2',ROOT/'tools/spectrum_test/test_spectrum.cpp','-o',exe])
    run([exe])
    usb()
    if args.ui: ui()
