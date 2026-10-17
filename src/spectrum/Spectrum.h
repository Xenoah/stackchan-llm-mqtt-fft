#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>
#if defined(STACKCHAN_USE_ESP_DSP)
#include "dsps_fft4r.h"
#include "esp_heap_caps.h"
#endif

// Pack L/R into one complex FFT. Mirrored powers preserve stereo energy even
// with opposite-phase channels (mono summing would incorrectly cancel them).
class Spectrum {
public:
    static constexpr int N=4096, Bands=32, Rate=48000;
    float db[Bands]={}, rms[2]={-120,-120}, peakHz=0, peakDb=-120;
    float waveform[256]={};
    bool clipping=false;
    Spectrum() {
        for(int i=0;i<N;++i) window[i]=0.5f-0.5f*std::cos(2*Pi*i/N);
        for(auto &v:db) v=-120;
        for(int b=0;b<Bands;++b) {
            bandLo[b]=std::max(1,int(std::ceil(edge(b)*N/Rate)));
            bandHi[b]=std::min(N/2,std::max(bandLo[b]+1,int(std::ceil(edge(b+1)*N/Rate))));
        }
    }
    bool begin() {
#if defined(STACKCHAN_USE_ESP_DSP)
        // Keep twiddles in internal RAM, not the default large-allocation PSRAM.
        auto table=static_cast<float*>(heap_caps_aligned_alloc(16,4*N*sizeof(float),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        if(!table) return false;
        if(dsps_fft4r_init_fc32(table,N)!=ESP_OK) { heap_caps_free(table); return false; }
#endif
        return true;
    }
    void clear() {
        for(auto &v:db) v=-120;
        rms[0]=rms[1]=peakDb=-120; peakHz=0; clipping=false;
        for(auto &v:waveform) v=0;
    }
    static float decibels(float a) { return 20*std::log10(std::max(a,1e-6f)); }
    static float edge(int b) { return 25.f*std::pow(800.f,float(b)/Bands); }
    void analyze(const int16_t *stereo) {
        float sums[2]={},means[2]={}; clipping=false;
        for(int i=0;i<N;++i) {
            float l=stereo[2*i]/32768.f,r=stereo[2*i+1]/32768.f;
            means[0]+=l; means[1]+=r; sums[0]+=l*l; sums[1]+=r*r;
            clipping|=std::abs(stereo[2*i])>=32760 || std::abs(stereo[2*i+1])>=32760;
        }
        for(int ch=0;ch<2;++ch) { means[ch]/=N; rms[ch]=decibels(std::sqrt(sums[ch]/N)); }
        for(int i=0;i<N;++i) {
            fft[2*i]=(stereo[2*i]/32768.f-means[0])*window[i];
            fft[2*i+1]=(stereo[2*i+1]/32768.f-means[1])*window[i];
        }
        for(int i=0;i<256;++i) waveform[i]=stereo[2*(N-512+i*2)]/32768.f;
#if defined(STACKCHAN_USE_ESP_DSP)
        dsps_fft4r_fc32(fft,N);
        dsps_bit_rev4r_fc32(fft,N);
#else
        // Portable backend shared with host tests. Keep Arduino's IDF 4.4
        // and leave internal RAM available for Wi-Fi/TLS and USB audio.
        for(int i=1,j=0;i<N;++i) {
            int bit=N>>1; for(;j&bit;bit>>=1) j^=bit; j^=bit;
            if(i<j) { std::swap(fft[2*i],fft[2*j]); std::swap(fft[2*i+1],fft[2*j+1]); }
        }
        for(int len=2;len<=N;len<<=1) {
            float angle=-2*Pi/len,wr0=std::cos(angle),wi0=std::sin(angle);
            for(int start=0;start<N;start+=len) {
                float wr=1,wi=0;
                for(int j=0;j<len/2;++j) {
                    int a=2*(start+j),b=a+len;
                    float tr=wr*fft[b]-wi*fft[b+1],ti=wr*fft[b+1]+wi*fft[b];
                    fft[b]=fft[a]-tr; fft[b+1]=fft[a+1]-ti; fft[a]+=tr; fft[a+1]+=ti;
                    float w=wr*wr0-wi*wi0; wi=wr*wi0+wi*wr0; wr=w;
                }
            }
        }
#endif
        float best=0; int bestBin=0;
        for(int k=1;k<N/2;++k) {
            int a=2*k,b=2*(N-k);
            power[k]=(fft[a]*fft[a]+fft[a+1]*fft[a+1]+fft[b]*fft[b]+fft[b+1]*fft[b+1])*(4.f/(N*N));
            if(power[k]>best) { best=power[k]; bestBin=k; }
        }
        peakDb=decibels(std::sqrt(best)); peakHz=0;
        if(peakDb>-90 && bestBin>1 && bestBin<N/2-1) {
            float a=std::log(std::max(power[bestBin-1],1e-20f));
            float b=std::log(std::max(power[bestBin],1e-20f));
            float c=std::log(std::max(power[bestBin+1],1e-20f));
            float denom=a-2*b+c,offset=std::abs(denom)>1e-9f?0.5f*(a-c)/denom:0;
            peakHz=(bestBin+std::max(-0.5f,std::min(0.5f,offset)))*Rate/N;
        } else if(peakDb>-90) peakHz=float(bestBin)*Rate/N;
        // Strongest FFT bin per logarithmic band; Hann coherent-gain corrected.
        for(int band=0;band<Bands;++band) {
            float p=0; for(int k=bandLo[band];k<bandHi[band];++k) p=std::max(p,power[k]);
            db[band]=decibels(std::sqrt(p));
        }
    }
private:
    static constexpr float Pi=3.14159265358979323846f;
    alignas(16) float fft[N*2];
    float window[N],power[N/2]={};
    int bandLo[Bands],bandHi[Bands];
};
