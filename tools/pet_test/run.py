"""Host checks for head gestures and the firmware's actual pet motion functions.

python tools/pet_test/run.py
Requires Python's ziglang package. Does not connect to or move hardware.
"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / '.pio/pet-test'


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


HARNESS = r'''
#include "PetReaction.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
using std::min; using std::max;
constexpr float PI = 3.14159265358979323846f;
void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
uint32_t tick = 100;
uint32_t millis() { return tick; }
PetReaction petReaction;
bool speaking = false, petSpeaking = false, levelHoldActive = false;
bool modeMenuOpen = false, settingsInfoOpen = false, deviceSetupOpen = false;
bool petInteractionReady = true, petFeedbackPending = false;
bool petRestorePrinterScreen = false, printerScreenOpen = false;
namespace usb_spectrum { bool open=false; bool isOpen() {return open;} }
enum class AppMode { LocalLlm, Printer, LevelHold };
AppMode currentMode = AppMode::Printer;
namespace m5avatar { enum class Expression { Happy }; }
struct {
  bool showcase = false;
  bool isShowcaseEnabled() { return showcase; }
  void toggleShowcase() { showcase = !showcase; }
  void setExpression(m5avatar::Expression) {}
  void showStatus(const char*,uint32_t) {}
  void showCaption(const char*,uint32_t) {}
  void returnToDefaultAfter(uint32_t) {}
} avatarFace;
struct { template<typename... T> void printf(const char*, T...) {} } Serial;
struct Calibration {
  struct Data { bool servoValid = true; } value;
  const Data& data() const { return value; }
} calibrationController;
struct { bool portal = false; bool isPortalActive() const { return portal; } } configPortal;
struct Hardware {
  struct MotionMock {
    struct Command { int yaw, pitch, speed; };
    std::vector<Command> commands;
    void move(int yaw,int pitch,int speed) { commands.push_back({yaw,pitch,speed}); }
  } Motion;
  struct TouchMock {
    std::array<uint8_t,3> values{};
    bool swipe = false;
    const std::array<uint8_t,3>& getIntensities() const { return values; }
    bool wasSwiped() { return swipe; }
  } TouchSensor;
} M5StackChan;
@BODY_STATE@
bool modeUsesBodyMotion() { return currentMode != AppMode::LevelHold; }
int clampBodyYaw(int v) { return std::clamp(v,-260,260); }
int clampBodyPitch(int v) { return std::clamp(v,120,280); }
int bodyHomeYaw() { return 0; }
int bodyHomePitch() { return 200; }
int starts = 0;
bool startBodyMotion(bool = false) { ++starts; bodyMotionState=BodyMotionState::Idle; return true; }
void showStatusLed(int,int,int,uint32_t = 1800) {}
int opened = 0;
void openPrinterScreen() { ++opened; printerScreenOpen=true; }
void closePrinterScreen() { printerScreenOpen=false; }
@FUNCTIONS@

void gesture(PetReaction& p,uint32_t start,bool forward=true) {
  p.update(start,0,false,true);
  p.update(start+10,forward?1:4,false,true);
  check(p.update(start+110,2,false,true),"partial stroke not recognized");
}
int main() {
 try {
  PetReaction p;
  p.update(0,0,false,true);
  for(uint32_t t=10;t<2000;t+=10) check(!p.update(t,2,false,true),"stationary hold became stroking");
  check(!p.active(2000),"hold activated joy");
  gesture(p,2100);
  check(p.level()==1 && p.takeSpeech(2210),"first stroke did not reply");
  check(!p.takeSpeech(2211),"duplicate reply");
  check(!p.update(2300,4,true,true),"BSP and partial gesture double-counted");
  gesture(p,2600,false);
  gesture(p,3100);
  check(p.level()==3,"repeated petting did not build excitement");
  check(!p.speechReady(3210),"speech queue flooded during cooldown");
  for(uint32_t t=3300;t<8200;t+=10) p.update(t,2,false,true);
  check(p.suppressesClicks(8200),"held stroke became showcase hold");
  check(!p.speechReady(8200),"stale queued reply survived unrelated speech");
  p.update(8300,0,false,true);
  check(p.suppressesClicks(8800) && !p.suppressesClicks(9100),"release click suppression timing");
  check(!p.active(9210),"joy never expired");
  gesture(p,9400);
  check(p.level()==1 && p.takeSpeech(9510),"new interaction did not reset excitement");
  p.update(9600,1,false,false);
  check(!p.active(9600) && !p.speechReady(9600),"settings did not cancel reaction");
  check(!p.update(9800,4,true,true),"touch begun in settings leaked out");
  gesture(p,10000);
  check(p.active(10110),"new touch after settings did not work");

  PetReaction wrap;
  gesture(wrap,UINT32_MAX-150);
  check(wrap.active(60) && wrap.takeSpeech(60),"millis wrap broke gesture/reply");
  check(!wrap.active(6100),"millis wrap prevented expiry");

  // Real main.cpp input/motion integration, hardware output mocked.
  printerScreenOpen=true;
  bodyMotionState=BodyMotionState::Idle;
  bodyMotionYawTarget=240; bodyMotionPitchTarget=260;
  tick=10010; M5StackChan.TouchSensor.values={1,0,0}; updatePetInteraction();
  tick=10110; M5StackChan.TouchSensor.values={0,1,0}; updatePetInteraction();
  check(!printerScreenOpen && petRestorePrinterScreen,"printer detail did not reveal happy face");
  check(bodyMotionState==BodyMotionState::Pet && starts==0,"stroke restarted servo controller");
  check(bodyMotionYawTarget==240 && bodyMotionPitchTarget==260,"stroke jumped to a new target");
  check(petReaction.takeSpeech(tick),"main input failed to schedule speech");
  speaking=petSpeaking=true;
  for(int i=0;i<70;++i) {
    tick+=80;
    if(i==10) {
      M5StackChan.TouchSensor.values={0,0,1}; M5StackChan.TouchSensor.swipe=true;
      updatePetInteraction(); M5StackChan.TouchSensor.swipe=false;
      check(petReaction.level()==2,"petting during speech was lost");
    }
    const int oldYaw=bodyMotionYawTarget, oldPitch=bodyMotionPitchTarget;
    updateBodyMotion();
    check(abs(bodyMotionYawTarget-oldYaw)<=48 && abs(bodyMotionPitchTarget-oldPitch)<=30,"pet trajectory jumped");
    check(bodyMotionYawTarget>=-260 && bodyMotionYawTarget<=260 &&
          bodyMotionPitchTarget>=120 && bodyMotionPitchTarget<=280,"motion escaped calibration limits");
  }
  check(M5StackChan.Motion.commands.size()==70,"motion stopped during pet speech");
  const auto count=M5StackChan.Motion.commands.size();
  petSpeaking=false; tick+=80; updateBodyMotion();
  check(M5StackChan.Motion.commands.size()==count,"motion ran during unrelated speech");
  speaking=false; modeMenuOpen=true; tick+=80; updatePetInteraction(); updateBodyMotion();
  check(!petRestorePrinterScreen && !petReaction.active(tick),"menu did not retain screen ownership");
  check(M5StackChan.Motion.commands.size()==count,"motion ran inside menu");
  modeMenuOpen=false;
  M5StackChan.TouchSensor.values={0,0,0}; tick+=80; updatePetInteraction(); updateBodyMotion();
  check(bodyMotionState==BodyMotionState::Idle && opened==0,"UI cancellation unexpectedly reopened printer");
  // A completed interaction restores the previously open detail screen.
  printerScreenOpen=true;
  tick+=1000; M5StackChan.TouchSensor.values={1,0,0}; updatePetInteraction();
  tick+=100; M5StackChan.TouchSensor.values={0,1,0}; updatePetInteraction();
  tick+=6100; M5StackChan.TouchSensor.values={0,0,0}; updatePetInteraction();
  check(printerScreenOpen && opened==1,"finished pet reaction did not restore detail screen");
  std::puts("PASS: gestures, hold/click isolation, excitement, reply coalescing/expiry, rollover, speech-time motion, limits, UI priority and screen restore");
  return 0;
 } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
'''


def main():
    source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
    state = source[source.index('enum class BodyMotionState'):source.index('// メニューとSettings情報画面')]
    functions = '\n'.join(function(source, signature) for signature in [
        'int moveTowardByStep(', 'bool bodyMotionCanUseServo(',
        'void triggerPetHappyMotion(', 'void updatePetInteraction() {', 'void updateBodyMotion('])
    CACHE.mkdir(parents=True, exist_ok=True)
    cpp, exe = CACHE / 'pet_test.cpp', CACHE / 'pet_test.exe'
    cpp.write_text(HARNESS.replace('@BODY_STATE@', state).replace('@FUNCTIONS@', functions), encoding='utf-8')
    subprocess.run([sys.executable, '-m', 'ziglang', 'c++', '-std=c++17', '-O1',
                    '-I'+str(ROOT/'src'), str(cpp), str(ROOT/'src/PetReaction.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
