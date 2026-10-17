#include "../../src/spectrum/Spectrum.h"
#include <cstdio>
#include <cstdlib>
static Spectrum fft;
static int16_t samples[Spectrum::N*2];
static void check(bool ok,const char *name) {
    std::printf("%s: %s\n",ok?"PASS":"FAIL",name); if(!ok) std::exit(1);
}
static void tone(float hz,float left,float right) {
    for(int i=0;i<Spectrum::N;++i) {
        float v=std::sin(6.28318530718f*hz*i/Spectrum::Rate);
        samples[2*i]=int16_t(32767*left*v); samples[2*i+1]=int16_t(32767*right*v);
    }
    fft.analyze(samples);
}
int main() {
    check(fft.begin(),"FFT initialization");
    fft.analyze(samples); check(fft.peakHz==0 && fft.peakDb<=-119,"digital silence");
    const float binTone=96.f*48000/4096;
    tone(binTone,0.5f,0.5f);
    check(std::abs(fft.peakDb+6.0206f)<0.05f,"Hann amplitude calibration");
    check(std::abs(fft.rms[0]+9.0309f)<0.05f,"RMS calibration");
    float normal=fft.peakDb;
    tone(binTone,0.5f,-0.5f); check(std::abs(fft.peakDb-normal)<0.01f,"opposite-phase stereo does not cancel");
    tone(binTone,0.5f,0); check(std::abs(fft.peakDb-normal+3.0103f)<0.05f,"single channel energy");
    for(float hz:{31.25f,100.f,440.f,1000.f,4000.f,16000.f,19950.f}) {
        tone(hz,0.25f,0.25f);
        std::printf("Tone %.2f -> %.2f Hz\n",hz,fft.peakHz);
        check(std::abs(fft.peakHz-hz)<3,"frequency estimation");
    }
    for(auto &s:samples) s=10000;
    fft.analyze(samples); check(fft.peakDb<=-110,"DC rejection");
    fft.clear(); check(fft.peakHz==0 && fft.rms[0]<=-119 && fft.db[0]<=-119,"idle reset clears held analysis");
    std::puts("All spectrum DSP tests passed.");
}
