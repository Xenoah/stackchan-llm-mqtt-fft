// FFT and differential renderer adapted from cores3-usb-spectrum v1.1.0.
// Copyright (c) 2026 Xenoah; MIT, see docs/licenses/cores3-usb-spectrum.txt.
#include "UsbSpectrum.h"
#include "spectrum/Spectrum.h"
#include "usb/UsbAudio.h"
#include <M5Unified.h>
#include <cstring>
#include <new>
#include "esp_heap_caps.h"
#include "esp_timer.h"

namespace usb_spectrum {
namespace {
Spectrum* spectrum=nullptr;
M5Canvas screen(&M5.Display);
// Small USB writes stay in internal RAM. Larger DSP scratch stays in PSRAM.
alignas(4) int16_t history[Spectrum::N*2]={};
int16_t* snapshot=nullptr;
portMUX_TYPE audioLock=portMUX_INITIALIZER_UNLOCKED;
unsigned writeFrame=0;
uint32_t receivedFrames=0,analyzedFrames=0,analysisCount=0;
int64_t lastAudioUs=0,lastAnalysisUs=0;
float shown[32]={},peaks[32]={},peakAge[32]={};
bool hold=false,waveformMode=false,active=false,newAnalysis=false;
bool ready=false,screenOpen=false,forceRedraw=true;
uint32_t frameNumber=0;
alignas(16) lgfx::swap565_t dmaPixels[2][320*8];
unsigned dmaIndex=0;
uint32_t frameBytes=0,transferUs=0;
struct Performance {
    int64_t start=0;
    uint64_t fftTotal=0,renderTotal=0,lcdTotal=0,byteTotal=0;
    uint32_t analyses=0,frames=0,maxWork=0;
    float fps=0;
    uint32_t fftUs=0,renderUs=0,lcdUs=0,bytesPerFrame=0,maxFrameUs=0;
    void record(int64_t now,uint32_t fft,uint32_t render,uint32_t lcd,uint32_t work,bool analyzed) {
        if(!start) start=now;
        if(analyzed) { fftTotal+=fft; ++analyses; }
        if(frameBytes) {
            ++frames; renderTotal+=render; lcdTotal+=lcd; byteTotal+=frameBytes;
            maxWork=std::max(maxWork,work);
        }
        if(now-start>=1000000) {
            fps=frames*1000000.f/(now-start);
            fftUs=analyses?fftTotal/analyses:0;
            renderUs=frames?renderTotal/frames:0; lcdUs=frames?lcdTotal/frames:0;
            bytesPerFrame=frames?byteTotal/frames:0; maxFrameUs=maxWork;
            start=now; fftTotal=renderTotal=lcdTotal=byteTotal=0;
            analyses=frames=maxWork=0;
        }
    }
} perf;
static constexpr uint16_t BG=0x0862,GRID=0x1926,TEXT=0xE73C,DIM=0x7C72;


void receiveAudio(const int16_t* samples,size_t frames) {
    if(!frames || frames>Spectrum::N) return;
    const int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&audioLock);
    if(now-lastAudioUs>200000) { memset(history,0,sizeof(history)); writeFrame=0; }
    unsigned first=std::min(unsigned(frames),unsigned(Spectrum::N)-writeFrame);
    memcpy(history+writeFrame*2,samples,first*4);
    if(first<frames) memcpy(history,samples+first*2,(frames-first)*4);
    writeFrame=(writeFrame+frames)%Spectrum::N;
    receivedFrames+=frames; lastAudioUs=now;
    portEXIT_CRITICAL(&audioLock);
}
static void captureAudio() {
    const int64_t now=esp_timer_get_time();
    newAnalysis=false;
    portENTER_CRITICAL(&audioLock);
    active=lastAudioUs && now-lastAudioUs<250000;
    if(active && !hold && receivedFrames!=analyzedFrames) {
        unsigned first=Spectrum::N-writeFrame;
        memcpy(snapshot,history+writeFrame*2,first*4);
        if(writeFrame) memcpy(snapshot+first*2,history,writeFrame*4);
        analyzedFrames=receivedFrames; newAnalysis=true;
    }
    portEXIT_CRITICAL(&audioLock);
    if(!hold && !active && lastAnalysisUs) {
        spectrum->clear(); lastAnalysisUs=0;
        for(int i=0;i<32;++i) shown[i]=peaks[i]=peakAge[i]=0;
    }
    if(newAnalysis) {
        spectrum->analyze(snapshot);
        ++analysisCount;
        const float dt=lastAnalysisUs?std::min(.25f,(now-lastAnalysisUs)*1e-6f):.016f;
        lastAnalysisUs=now;
        const float attack=1.f-std::exp(-dt/.04f);
        for(int i=0;i<32;++i) {
            float target=std::max(0.f,std::min(1.f,(spectrum->db[i]+72.f)/72.f));
            shown[i]=target>shown[i]?shown[i]+attack*(target-shown[i]):std::max(target,shown[i]-.4f*dt);
            if(shown[i]>=peaks[i]) { peaks[i]=shown[i]; peakAge[i]=0; }
            else if((peakAge[i]+=dt)>1.2f) peaks[i]=std::max(shown[i],peaks[i]-.2f*dt);
        }
    }
}
static uint16_t barColor(int y) {
    if(y<95) return screen.color565(255,108,113);
    if(y<130) return screen.color565(252,209,104);
    return screen.color565(71,221,201);
}
static void label(const char *s,int x,int y,uint16_t color=DIM,int size=1) {
    screen.setTextColor(color,BG); screen.setTextSize(size); screen.setCursor(x,y); screen.print(s);
}
static void renderFullScene() {
    screen.fillScreen(BG);
    label("SPECTRUM",12,10,TEXT,2);
    screen.fillCircle(218,15,3,active?0x5EF5:0x8410);
    label(active?"USB 48K":"USB WAIT",229,12,active?0x5EF5:DIM);
    label("<",307,12,TEXT);
    screen.drawFastHLine(12,33,296,GRID);
    char line[48];
    snprintf(line,sizeof(line),"PEAK %7.1f Hz",spectrum->peakHz); label(line,12,42,TEXT);
    label(hold?"HOLD":spectrum->clipping?"CLIP":"LIVE",270,42,hold?0xFEA8:spectrum->clipping?0xFAAA:0x5EF5);
    for(int ch=0;ch<2;++ch) {
        int x=12+ch*151; label(ch?"R":"L",x,58);
        screen.fillRoundRect(x+12,57,130,5,2,GRID);
        int w=int(std::max(0.f,std::min(1.f,(spectrum->rms[ch]+60.f)/60.f))*130);
        if(w) screen.fillRoundRect(x+12,57,w,5,2,spectrum->rms[ch]>-3?0xFAAA:0x5EF5);
    }
    for(int j=0;j<=3;++j) {
        int y=76+j*40; screen.drawFastHLine(28,y,280,GRID);
        snprintf(line,sizeof(line),"%d",-24*j); label(line,2,y-3);
    }
    if(!waveformMode) {
        for(int i=0;i<32;++i) {
            int x=29+i*280/32,h=int(shown[i]*120);
            for(int dy=0;dy<h;dy+=4) screen.fillRect(x,196-dy-3,6,std::min(3,h-dy),barColor(196-dy));
            if(peaks[i]>0.01f) screen.drawFastHLine(x,196-int(peaks[i]*120),6,TEXT);
        }
        label("25",28,203); label("100",78,203); label("1k",176,203); label("20k Hz",270,203);
    } else {
        screen.drawFastHLine(28,136,280,0x43AB);
        for(int x=1;x<256;++x) screen.drawLine(28+x-1,136-int(spectrum->waveform[x-1]*56),28+x,136-int(spectrum->waveform[x]*56),0x5EF5);
        label("L WAVEFORM / 10.7 ms",70,203);
    }
    if(!active && !hold) {
        screen.fillRoundRect(53,109,233,47,5,BG);
        label(usb_audio::mounted()?"Select PC sound output:":"Connect USB to your PC",65,119,TEXT);
        label("StackChan USB Spectrum",67,137,0x5EF5);
    }
    const char *buttons[]={waveformMode?"BARS":"WAVE",hold?"RESUME":"HOLD",usb_audio::localMute()?"SPK OFF":"SPK ON"};
    for(int i=0;i<3;++i) {
        screen.fillRoundRect(9+i*104,219,96,18,4,GRID);
        screen.setTextSize(1); screen.setTextColor(i==2&&usb_audio::localMute()?DIM:TEXT,GRID);
        screen.drawCenterString(buttons[i],57+i*104,224);
    }
}

static void presentRect(int x,int y,int w,int h) {
    const int64_t begin=esp_timer_get_time();
    const auto *pixels=static_cast<const lgfx::swap565_t*>(screen.getBuffer());
    const int rowsPerChunk=int(sizeof(dmaPixels[0])/sizeof(dmaPixels[0][0]))/w;
    for(int row=0;row<h;row+=rowsPerChunk) {
        const int rows=std::min(rowsPerChunk,h-row);
        auto *buffer=dmaPixels[dmaIndex];
        for(int dy=0;dy<rows;++dy)
            memcpy(buffer+dy*w,pixels+(y+row+dy)*320+x,w*sizeof(*pixels));
        M5.Display.waitDMA();
        M5.Display.pushImageDMA(x,y+row,w,rows,buffer);
        dmaIndex^=1;
    }
    frameBytes+=w*h*2;
    transferUs+=esp_timer_get_time()-begin;
}

static int meterWidth(int ch) {
    return int(std::max(0.f,std::min(1.f,(spectrum->rms[ch]+60.f)/60.f))*130);
}
static int peakHeight(int band) { return peaks[band]>.01f?int(peaks[band]*120):-1; }

static void drawScreen() {
    static int oldScene=-1,oldHeight[32],oldPeak[32],oldMeter[2],oldFps=-1;
    static int oldWaveTop=76,oldWaveBottom=196;
    static bool oldRed[2];
    static char oldPeakText[48]={};
    static bool oldClip=false;
    const int scene=int(active)|(int(hold)<<1)|(int(waveformMode)<<2)
        |(int(usb_audio::localMute())<<3)|(int(usb_audio::mounted())<<4);
    const bool full=forceRedraw || scene!=oldScene;
    forceRedraw=false;
    frameBytes=transferUs=0;
    char line[48]; snprintf(line,sizeof(line),"PEAK %7.1f Hz",spectrum->peakHz);
    M5.Display.startWrite();
    if(full) {
        renderFullScene();
        for(int i=0;i<32;++i) { oldHeight[i]=int(shown[i]*120); oldPeak[i]=peakHeight(i); }
        for(int ch=0;ch<2;++ch) { oldMeter[ch]=meterWidth(ch); oldRed[ch]=spectrum->rms[ch]>-3; }
        strcpy(oldPeakText,line); oldClip=spectrum->clipping;
        oldScene=scene; oldFps=-1; oldWaveTop=76; oldWaveBottom=196;
    } else if(newAnalysis) {
        if(strcmp(line,oldPeakText)) {
            screen.fillRect(12,42,156,8,BG); label(line,12,42,TEXT);
            presentRect(12,42,156,8); strcpy(oldPeakText,line);
        }
        if(spectrum->clipping!=oldClip) {
            screen.fillRect(270,42,30,8,BG);
            label(spectrum->clipping?"CLIP":"LIVE",270,42,spectrum->clipping?0xFAAA:0x5EF5);
            presentRect(270,42,30,8); oldClip=spectrum->clipping;
        }
        for(int ch=0;ch<2;++ch) {
            int w=meterWidth(ch); bool red=spectrum->rms[ch]>-3;
            if(w==oldMeter[ch] && red==oldRed[ch]) continue;
            int x=24+151*ch;
            screen.fillRoundRect(x,57,130,5,2,GRID);
            if(w) screen.fillRoundRect(x,57,w,5,2,red?0xFAAA:0x5EF5);
            presentRect(x,57,130,5); oldMeter[ch]=w; oldRed[ch]=red;
        }
        if(waveformMode) {
            // Include both old and new extents so a shrinking trace leaves no
            // stale pixels. The rest of the grid and centerline stay on the LCD.
            int top=136,bottom=136;
            for(float sample:spectrum->waveform) {
                int y=136-int(sample*56);
                top=std::min(top,y); bottom=std::max(bottom,y);
            }
            int dirtyTop=std::min(top,oldWaveTop),dirtyBottom=std::max(bottom,oldWaveBottom);
            screen.fillRect(28,dirtyTop,256,dirtyBottom-dirtyTop+1,BG);
            for(int j=0;j<=3;++j) {
                int y=76+j*40;
                if(y>=dirtyTop && y<=dirtyBottom) screen.drawFastHLine(28,y,256,GRID);
            }
            screen.drawFastHLine(28,136,256,0x43AB);
            for(int x=1;x<256;++x)
                screen.drawLine(28+x-1,136-int(spectrum->waveform[x-1]*56),28+x,136-int(spectrum->waveform[x]*56),0x5EF5);
            presentRect(28,dirtyTop,256,dirtyBottom-dirtyTop+1);
            oldWaveTop=top; oldWaveBottom=bottom;
        } else for(int i=0;i<32;++i) {
            int h=int(shown[i]*120),p=peakHeight(i);
            if(h==oldHeight[i] && p==oldPeak[i]) continue;
            int x=29+i*280/32;
            int top=std::max(76,196-std::max({h+2,oldHeight[i]+2,p,oldPeak[i]}));
            screen.fillRect(x,76,6,121,BG);
            for(int j=0;j<=3;++j) screen.drawFastHLine(x,76+j*40,6,GRID);
            for(int dy=0;dy<h;dy+=4)
                screen.fillRect(x,196-dy-3,6,std::min(3,h-dy),barColor(196-dy));
            if(p>=0) screen.drawFastHLine(x,196-p,6,TEXT);
            presentRect(x,top,6,197-top);
            oldHeight[i]=h; oldPeak[i]=p;
        }
    }
    int fps=int(perf.fps+.5f);
    if(fps!=oldFps) {
        screen.fillRect(158,10,54,10,BG);
        char rate[16]; snprintf(rate,sizeof(rate),"%3dFPS",fps); label(rate,158,12,DIM);
        if(!full) presentRect(158,10,54,10);
        oldFps=fps;
    }
    if(full) presentRect(0,0,320,240);
    const int64_t waitStart=esp_timer_get_time();
    M5.Display.waitDMA(); M5.Display.endWrite();
    transferUs+=esp_timer_get_time()-waitStart;
    // Count completed LCD updates, not iterations that drew nothing.
    if(frameBytes) ++frameNumber;
}

}  // namespace

bool begin() {
    if(ready) return true;
    void* memory=heap_caps_malloc(sizeof(Spectrum),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    snapshot=static_cast<int16_t*>(heap_caps_calloc(Spectrum::N*2,sizeof(int16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!memory || !snapshot) {
        heap_caps_free(memory); heap_caps_free(snapshot); snapshot=nullptr; return false;
    }
    spectrum=new(memory) Spectrum();
    if(!spectrum->begin() || !usb_audio::begin(receiveAudio)) {
        spectrum->~Spectrum(); heap_caps_free(spectrum); spectrum=nullptr;
        heap_caps_free(snapshot); snapshot=nullptr; return false;
    }
    ready=true;
    return true;
}

void update(bool homeVisible) {
    if(!ready) return;
    static uint32_t lastUpdate=0;
    const uint32_t now=millis();
    // Share CPU with face animation, Wi-Fi, MQTT and TTS. USB playback is on
    // its own task and continues even when serviceApp is in a blocking UI.
    if(now-lastUpdate<33) return;
    lastUpdate=now;
    if(!screenOpen && !homeVisible) { active=usb_audio::active(); return; }
    const int64_t start=esp_timer_get_time();
    captureAudio();
    const int64_t analyzed=esp_timer_get_time();
    if(screenOpen) {
        drawScreen();
        const int64_t end=esp_timer_get_time();
        perf.record(end,analyzed-start,end-analyzed-transferUs,transferUs,end-start,newAnalysis);
    }
}

bool open() {
    if(!ready) return false;
    if(!screen.getBuffer()) {
        screen.setPsram(true); screen.setColorDepth(16);
        if(!screen.createSprite(320,240)) return false;
    }
    screen.setFont(&fonts::Font0); screen.setTextDatum(top_left);
    screen.setTextWrap(false);
    screenOpen=true; forceRedraw=true;
    captureAudio(); drawScreen();
    return true;
}
void close() {
    screenOpen=false; hold=false;
    // drawScreen always waits for DMA before returning.
    screen.deleteSprite();
}
bool isOpen() { return screenOpen; }
bool isReady() { return ready; }
bool isActive() { return ready && active; }
void tap(int16_t x,int16_t y) {
    if(!screenOpen || y<214) return;
    if(x<109) waveformMode=!waveformMode;
    else if(x<213) hold=!hold;
    else usb_audio::setLocalMute(!usb_audio::localMute());
    forceRedraw=true;
}
void copyBars(uint8_t (&bars)[32]) {
    for(int i=0;i<32;++i) bars[i]=uint8_t(std::max(0.f,std::min(1.f,shown[i]))*100);
}
}  // namespace usb_spectrum
