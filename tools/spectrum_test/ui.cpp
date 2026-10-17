// Host UI checks use actual firmware rendering and real M5GFX fonts.
#include <M5Unified.h>
#include <fstream>
#include <vector>
#include <stdexcept>
#include "FaceHud.h"
#include "Mouth.h"
#include "Eye.h"
#include "Eyeblow.h"
#include "PrinterState.h"
#include "SetupUi.h"
#include "UsbSpectrum.cpp"
#include "menu_types.inc"

namespace usb_audio {
Receiver sink = nullptr;
bool muted = false;
bool begin(Receiver r) { sink = r; return true; }
bool mounted() { return true; }
bool active() { return usb_spectrum::active; }
void setLocalMute(bool v) { muted = v; }
bool localMute() { return muted; }
void setSpeechActive(bool) {}
uint32_t droppedBytes() { return 0; }
}
PrinterState printerNow;
AppMode currentMode = AppMode::LocalLlm;
bool printerModeAuto = false;
uint32_t modeMenuHintUntil = 0;
String modeMenuHint;
struct { bool isEnabled() const { return true; } } bambu;
struct HostConfig { bool commentaryVoice = true, autoPrinterMode = true; };
struct { HostConfig c; const HostConfig& config() const { return c; } } configPortal;
M5Canvas& startupCanvas() { return M5.Display; }
#include "menu_draw.inc"

void check(bool ok,const char* message) {
  if(!ok) throw std::runtime_error(message);
}
void save(M5Canvas& c,const std::string& name) {
  std::vector<lgfx::bgr888_t> pixels(320*240);
  c.readRectRGB(0,0,320,240,pixels.data());
  std::ofstream file(name,std::ios::binary); file<<"P6\n320 240\n255\n";
  for(auto& p:pixels) { file.put(p.R8()); file.put(p.G8()); file.put(p.B8()); }
  check(bool(file),"image output");
}
void compareLcd() {
  for(int y=0;y<240;++y) for(int x=0;x<320;++x)
    check(M5.Display.readPixel(x,y)==usb_spectrum::screen.readPixel(x,y),"differential LCD pixels differ from framebuffer");
}
void feed(float hz,float amplitude) {
  static int16_t data[Spectrum::N*2];
  for(int i=0;i<Spectrum::N;++i)
    data[i*2]=data[i*2+1]=int16_t(amplitude*32767*std::sin(6.28318530718f*hz*i/48000));
  captureNow+=90; usb_audio::sink(data,Spectrum::N); usb_spectrum::update(true);
}
int main(int argc,char** argv) {
 try {
  check(argc==2,"output path"); const std::string out=argv[1];
  M5.Display.setColorDepth(8); M5.Display.createSprite(320,240);
  for(size_t i=0;i<sizeof(kMenuOrder)/sizeof(kMenuOrder[0]);++i) {
    int16_t x,y,w,h; check(modeMenuTileRect(i,x,y,w,h),"menu rectangle");
    check(x>=0 && y>=0 && x+w<=320 && y+h<=240,"menu inside display");
    check(modeMenuButtonAt(x+w/2,y+h/2)==kMenuOrder[i],"menu hit testing");
  }
  check(usb_spectrum::begin(),"spectrum initializes");
  drawModeMenu(ModeMenuButton::None); save(M5.Display,out+"/menu.ppm");
  M5.Display.deleteSprite(); M5.Display.setColorDepth(16); M5.Display.createSprite(320,240);
  check(usb_spectrum::open(),"FFT opens"); compareLcd(); save(M5.Display,out+"/fft-idle.ppm");
  feed(1000,.5f); compareLcd(); save(M5.Display,out+"/fft-bars.ppm");
  check(std::abs(usb_spectrum::spectrum->peakHz-1000)<3,"live spectrum receives audio");
  for(float amp:{.2f,.8f,.01f,.9f}) { feed(440,amp); compareLcd(); }
  usb_spectrum::tap(50,225); captureNow+=40; usb_spectrum::update(true); compareLcd();
  save(M5.Display,out+"/fft-wave.ppm");
  for(float amp:{.8f,.1f,.0f}) { feed(4000,amp); compareLcd(); }
  usb_spectrum::tap(150,225); captureNow+=40; usb_spectrum::update(true);
  const float held=usb_spectrum::spectrum->peakHz;
  feed(1000,.5f); check(usb_spectrum::spectrum->peakHz==held,"HOLD freezes analysis"); compareLcd();
  captureNow+=300; usb_spectrum::update(true); compareLcd();
  usb_spectrum::tap(150,225); captureNow+=40; usb_spectrum::update(true); compareLcd();
  check(!usb_spectrum::isActive() && usb_spectrum::spectrum->peakHz==0,"RESUME without input clears stale graph");
  usb_spectrum::tap(270,225); captureNow+=40; usb_spectrum::update(true);
  check(usb_audio::localMute(),"SPK button toggles mute"); compareLcd();
  usb_spectrum::close(); check(!usb_spectrum::isOpen(),"close returns LCD ownership");
  feed(1000,.5f);
  uint8_t bars[32]; usb_spectrum::copyBars(bars);
  FaceHud hud; hud.setSpectrum(true,bars);
  check(hud.hasSpectrum(),"home graph enabled");
  M5Canvas c; c.setColorDepth(1); c.setBitmapColor(TFT_WHITE,TFT_BLACK); c.createSprite(320,240);
  c.fillSprite(0);
  using namespace m5avatar;
  ColorPalette palette;
  DrawContext ctx(Expression::Happy,0,&palette,Gaze(),1,Gaze(),1,.35f,"",BatteryIconStatus::invisible,0,nullptr);
  @PARTS@
  save(c,out+"/home-fft.ppm");
  int pixels=0; for(int y=164;y<202;++y) for(int x=0;x<320;++x) pixels+=c.readPixel(x,y)!=0;
  check(pixels>320,"spectrum drawn through actual face mouth");
  hud.setCaption("USB\xe9\x9f\xb3\xe5\xa3\xb0\xe3\x81\xa8\xe4\xb8\x80\xe7\xb7\x92\xe3\x81\xab\xe8\xa9\xb1\xe3\x81\x99\xe3\x82\x88",0);
  hud.draw(&c,1,1,0); save(c,out+"/home-caption.ppm");
  hud.setSpectrum(false,bars); hud.clearCaption();
  c.fillSprite(0); hud.draw(&c,1,1,0);
  for(int y=0;y<240;++y) for(int x=0;x<320;++x) check(c.readPixel(x,y)==0,"hidden graph leaves no stale pixels");
  check(usb_spectrum::open(),"FFT can reopen"); compareLcd(); usb_spectrum::close();
  puts("PASS: menu hit targets, live/idle FFT, differential LCD bars/wave, HOLD/RESUME, mute, reopen, face graph and caption");
 } catch(const std::exception& e) { fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
