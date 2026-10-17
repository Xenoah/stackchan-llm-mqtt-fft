"""Render real face parts + Japanese captions; check visibility and expiry.

Uses the same prerequisites as capture_device.py. No hardware is accessed.
Without --version, checks the working tree and saves previews to .pio/caption-test.
With --version, uses the Git tag and saves reproducible documentation images.
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
from capture_device import ROOT, HERE, SDL, GFX, compile_one, library, git_source

AVATAR = ROOT / '.pio/libdeps/m5stack-cores3/M5Stack-Avatar/src'
HARNESS = r'''
#include <M5Unified.h>
#include "FaceHud.h"
#include "Mouth.h"
#include "Eye.h"
#include "Eyeblow.h"
#include "Effect.h"
#include <fstream>
#include <vector>
#include <stdexcept>
using namespace m5avatar;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
int pixels(M5Canvas& c, int top, int bottom) {
  int count=0;
  for(int y=top;y<bottom;++y) for(int x=0;x<320;++x) count += c.readPixel(x,y)!=0;
  return count;
}
void save(M5Canvas& c, const std::string& file) {
  std::vector<lgfx::bgr888_t> data(320*240);
  c.readRectRGB(0,0,320,240,data.data());
  std::ofstream f(file,std::ios::binary); f<<"P6\n320 240\n255\n";
  for(const auto& p:data) {f.put(p.R8());f.put(p.G8());f.put(p.B8());}
  check(bool(f),"write image failed");
}
void scene(M5Canvas& c, FaceHud& hud, const std::string& file) {
  c.fillSprite(0);
  ColorPalette palette;
  DrawContext ctx(Expression::Happy,0,&palette,Gaze(),1,Gaze(),1,0.35f,"",
                  BatteryIconStatus::invisible,0,nullptr);
  @PARTS@
  Effect effect; effect.draw(&c,BoundingRect(),&ctx);
  check(pixels(c,206,240)>250,"caption missing through HudMouth");
  save(c,file);
}
int main(int argc,char** argv) {
 try {
  if(argc!=2) return 2;
  const std::string out=argv[1];
  M5Canvas c; c.setColorDepth(1); c.setBitmapColor(TFT_WHITE,TFT_BLACK);
  check(c.createSprite(320,240)!=nullptr,"sprite allocation failed");
  FaceHud hud;
  hud.setToast("HAPPY!",6000); // A status must not enable a hidden printer HUD.
  c.fillSprite(0); hud.draw(&c,1,1,0);
  check(pixels(c,0,240)==0,"hidden HUD leaked information");
  hud.setCaption(@GENTLE@,0);
  check(hud.hasCaption(),"caption incorrectly tied to printer visibility");
  c.fillSprite(0); hud.draw(&c,1,1,0);
  check(pixels(c,0,202)==0 && pixels(c,206,240)>250,"local caption missing or printer info leaked");
  scene(c,hud,out+"/device-pet-local.ppm");
  captureNow+=30000;
  check(hud.hasCaption(),"caption expired during speech");
  hud.setCaption(@GENTLE@,2500);
  captureNow+=2500;
  c.fillSprite(0); hud.draw(&c,1,1,0);
  check(!hud.hasCaption() && pixels(c,0,240)==0,"caption did not expire after speech");
  hud.setCaption(@EXCITED@,6000);
  HudData d; d.visible=true; d.active=true; d.percent=62;
  std::strcpy(d.phase,"印刷中"); std::strcpy(d.remaining,"0:42"); hud.set(d);
  scene(c,hud,out+"/device-pet-mqtt.ppm");
  check(pixels(c,0,38)>250,"MQTT top bar disappeared");
  hud.clearCaption(); check(!hud.hasCaption(),"clearCaption failed");
  d.visible=false; hud.set(d);
  captureNow=UINT32_MAX-100;
  hud.setCaption(@GENTLE@,2500);
  captureNow=100; check(hud.hasCaption(),"millis wrap lost caption");
  captureNow=2500; check(!hud.hasCaption(),"millis wrap kept expired caption");
  std::puts("PASS: Japanese caption without printer HUD, mouth integration, speech hold, expiry, MQTT bar and millis rollover");
 } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version')
    args = parser.parse_args()
    work = ROOT / '.pio/caption-test' / (args.version or 'worktree')
    work.mkdir(parents=True, exist_ok=True)
    out = ROOT / 'docs/screenshots' / args.version if args.version else work
    out.mkdir(parents=True, exist_ok=True)
    hashes = {}
    def source(path):
        text = git_source(args.version,path) if args.version else (ROOT/path).read_text(encoding='utf-8')
        hashes[path] = hashlib.sha256(text.encode()).hexdigest()
        return text
    for name in ['FaceHud.h','FaceHud.cpp']:
        (work/name).write_text(source('src/'+name),encoding='utf-8')
    main_source=source('src/main.cpp')
    gentle=re.search(r'gentle\[\]\s*=\s*\{\s*("[^"]+")',main_source).group(1)
    excited=re.search(r'excited\[\]\s*=\s*\{\s*"[^"]+",\s*("[^"]+")',main_source).group(1)
    # Copy the host adapter and replace only the clock/critical section I/O.
    arduino=(HERE/'native/Arduino.h').read_text(encoding='utf-8')
    arduino=arduino.replace('inline uint32_t millis() { return 10000; }',
        'inline uint32_t captureNow = 10000;\ninline uint32_t millis() { return captureNow; }')
    arduino+='\nusing portMUX_TYPE = int;\n#define portMUX_INITIALIZER_UNLOCKED 0\n'
    arduino+='#define portENTER_CRITICAL(x) ((void)(x))\n#define portEXIT_CRITICAL(x) ((void)(x))\n'
    arduino+='#define M5_LOGI(...) ((void)0)\ntemplate<class T> T constrain(T n,T lo,T hi) {return std::clamp(n,lo,hi);}\n'
    (work/'Arduino.h').write_text(arduino,encoding='utf-8')
    shutil.copy2(HERE/'native/M5Unified.h',work/'M5Unified.h')
    (work/'M5GFX.h').write_text('#pragma once\n#include "M5Unified.h"\n',encoding='utf-8')
    face=(AVATAR/'Face.cpp').read_text(encoding='utf-8')
    constructor=face[face.index('Face::Face()'):face.index('Face::Face(Drawable')]
    parts=re.findall(r'new (Mouth|Eye|Eyeblow)\(([^)]+)\),\s*new BoundingRect\(([^)]+)\)',constructor)
    assert len(parts)==5
    draw=[]
    for i,(kind,params,rect) in enumerate(parts):
        create=f'HudMouth part{i}(new Mouth({params}),&hud);' if kind=='Mouth' else f'{kind} part{i}({params});'
        draw.append(create+f' part{i}.draw(&c,BoundingRect({rect}),&ctx);')
    harness=HARNESS.replace('@PARTS@','\n'.join(draw)).replace('@GENTLE@',gentle).replace('@EXCITED@',excited)
    (work/'render.cpp').write_text(harness,encoding='utf-8')
    objects=library()
    names=['BoundingRect.cpp','ColorPalette.cpp','DrawContext.cpp','Gaze.cpp','Eye.cpp','Mouth.cpp','Eyeblow.cpp']
    adapter=work/'avatar'
    adapter.mkdir(exist_ok=True)
    for path in [*AVATAR.glob('*.h'),*(AVATAR/n for n in names)]:
        text=path.read_text(encoding='utf-8')
        if path.name=='DrawContext.h':
            # Use the same host String as the firmware adapter, not a second typedef.
            text=text.replace('#ifndef ARDUINO\n#include <string>\ntypedef std::string String;\n#endif  // ARDUINO',
                              '#include "Arduino.h"')
        (adapter/path.name).write_text(text,encoding='utf-8')
    defines=['-I'+str(work),'-I'+str(adapter)]
    for path in [*(adapter/n for n in names),work/'FaceHud.cpp',work/'render.cpp']:
        obj=work/(path.stem+'.o')
        compile_one(path,obj,True,defines)
        objects.append(obj)
    exe=work/'render.exe'
    subprocess.run([sys.executable,'-m','ziglang','c++','-O1',*map(str,objects),
        str(SDL/'lib/libSDL2.dll.a'),'-o',str(exe)],check=True,cwd=ROOT)
    shutil.copy2(SDL/'bin/SDL2.dll',work/'SDL2.dll')
    subprocess.run([str(exe),str(work)],check=True,env={**os.environ,'SDL_VIDEODRIVER':'dummy'})
    images=[]
    for name in ['device-pet-local','device-pet-mqtt']:
        path=out/(name+'.png')
        with Image.open(work/(name+'.ppm')) as im:
            assert im.size==(320,240)
            im.save(path,optimize=True)
        images.append({'file':path.name,'width':320,'height':240,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    dependencies={str(p.relative_to(AVATAR)):hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(AVATAR.glob('*.h'))+sorted(AVATAR.glob('*.cpp'))}
    (out/'caption-capture.json').write_text(json.dumps({
        'version':args.version or 'working tree',
        'commit':subprocess.check_output(['git','rev-parse',args.version],cwd=ROOT).decode().strip() if args.version else None,
        'capture':'Reproduction with actual FaceHud, M5GFX fonts and m5stack-avatar face parts; no hardware capture',
        'source_sha256':hashes,'avatar_source_sha256':dependencies,
        'renderer':{'M5GFX':'0.2.30','m5stack-avatar':'0.10.0','font':'lgfxJapanGothicP_16','color_depth':1},
        'sample':{'expression':'Happy','breath':0,'mouth_open_ratio':0.35,'percent':62,'remaining':'0:42'},
        'images':images},ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f'Rendered Japanese caption previews: {out}',flush=True)

if __name__=='__main__':
    main()
