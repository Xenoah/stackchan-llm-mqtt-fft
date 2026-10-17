// Offline checks compile the actual descriptor and UAC2 request handlers.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <vector>
#include <stdexcept>
#include "tusb.h"
#include "class/cdc/cdc.h"
constexpr uint8_t kClock=4,kInput=1,kFeature=2,kOutput=3;
uint8_t streamInterface=0;
uint8_t outEndpoint=0,feedbackEndpoint=0;
std::atomic<bool> hostMuted{false};
std::atomic<int16_t> volumeDb{0};
std::atomic<uint32_t> lastPacketMs{0};
std::vector<uint8_t> replyBytes;
bool noEndpoint=false;
uint8_t tinyusb_get_free_out_endpoint() {return noEndpoint?0:1;}
uint8_t tinyusb_get_free_in_endpoint() {return noEndpoint?0:1;}
uint8_t tinyusb_get_free_duplex_endpoint() {return noEndpoint?0:1;}
uint8_t tinyusb_add_string_descriptor(const char*) {return 6;}
extern "C" bool tud_audio_buffer_and_schedule_control_xfer(uint8_t,tusb_control_request_t const*,void* p,uint16_t n) {
  auto bytes=static_cast<uint8_t*>(p); replyBytes.assign(bytes,bytes+n); return true;
}
#include "usb_handlers.inc"
std::vector<uint8_t> configuration;
extern "C" const uint8_t* __real_tud_descriptor_configuration_cb(uint8_t) {
  return configuration.empty() ? nullptr : configuration.data();
}
#include "UsbDescriptorCompat.cpp"
void check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
uint16_t u16(const uint8_t* p) {return p[0]|(p[1]<<8);}
uint32_t u32(const uint8_t* p) {return u16(p)|(uint32_t(u16(p+2))<<16);}
int main() {
 try {
  uint8_t data[512]={},itf=2;
  const auto size=loadDescriptor(data,&itf);
  check(size==CFG_TUD_AUDIO_FUNC_1_DESC_LEN && itf==4,"two interfaces after CDC");
  check(data[1]==TUSB_DESC_INTERFACE_ASSOCIATION && data[2]==2 && data[3]==2,"UAC IAD binds AC and AS");
  int interfaces=0,out=0,feedback=0,format=0;
  for(size_t pos=0;pos<size;) {
    const uint8_t* p=data+pos;
    check(p[0]>=2 && pos+p[0]<=size,"well-formed descriptor length");
    if(p[1]==TUSB_DESC_INTERFACE) {
      ++interfaces;
      check(p[2]>=2 && p[2]<=3 && p[5]==TUSB_CLASS_AUDIO,"audio interfaces do not overlap CDC");
    }
    if(p[1]==TUSB_DESC_ENDPOINT) {
      if(p[2]==1) {++out;check(p[3]==5 && u16(p+4)==196 && p[6]==1,"async stereo OUT capacity");}
      else {++feedback;check(p[2]==0x81 && p[3]==0x11 && u16(p+4)==4 && p[6]==1,"explicit 16.16 feedback every frame");}
    }
    if(p[1]==TUSB_DESC_CS_INTERFACE && p[2]==AUDIO_CS_AS_INTERFACE_FORMAT_TYPE && p[0]==6) {
      ++format;check(p[3]==1 && p[4]==2 && p[5]==16,"PCM 16-bit format");
    }
    pos+=p[0];
  }
  check(interfaces==3 && out==1 && feedback==1 && format==1,"AC plus zero/streaming alternate; no microphone");
  const uint8_t cdc[] = {
    TUD_CONFIG_DESCRIPTOR(1,4,0,TUD_CONFIG_DESC_LEN+TUD_CDC_DESC_LEN+CFG_TUD_AUDIO_FUNC_1_DESC_LEN,0,500),
    TUD_CDC_DESCRIPTOR(0,0,0x85,64,0x03,0x84,64)
  };
  configuration.assign(cdc,cdc+sizeof(cdc));
  configuration.insert(configuration.end(),data,data+size);
  const uint8_t* combined=__wrap_tud_descriptor_configuration_cb(0);
  check(combined && u16(combined+2)==configuration.size(),"complete CDC/audio descriptor length");
  unsigned endpointMask=0,endpointCount=0;
  for(size_t pos=0;pos<configuration.size();pos+=combined[pos]) {
    const uint8_t* p=combined+pos;
    if(p[1]!=TUSB_DESC_ENDPOINT) continue;
    unsigned ep=p[2]&15,dir=p[2]>>7;
    check(ep>0 && ep<4,"all endpoints fit physical DWC2 TX FIFOs");
    unsigned bit=1u<<(ep+dir*8);
    check(!(endpointMask&bit),"CDC and audio endpoint addresses are unique");
    endpointMask|=bit;++endpointCount;
  }
  check(endpointCount==5 && endpointMask==((1u<<1)|(1u<<2)|(1u<<9)|(1u<<10)|(1u<<11)),"audio pair 1, CDC pair 2 and notification 3");
  check(!memcmp(combined+sizeof(cdc),data,size),"CDC remap preserves every audio descriptor");
  noEndpoint=true; itf=2; check(loadDescriptor(data,&itf)==0 && itf==2,"endpoint exhaustion rejected");
  tusb_control_request_t req={}; req.wIndex=uint16_t(kClock)<<8;req.wValue=uint16_t(AUDIO_CS_CTRL_SAM_FREQ)<<8;
  req.bRequest=AUDIO_CS_REQ_CUR;
  check(tud_audio_get_req_entity_cb(0,&req) && replyBytes.size()==4 && u32(replyBytes.data())==48000,"fixed 48kHz clock");
  req.bRequest=AUDIO_CS_REQ_RANGE;
  check(tud_audio_get_req_entity_cb(0,&req) && replyBytes.size()==14 && u16(replyBytes.data())==1 && u32(replyBytes.data()+2)==48000 && u32(replyBytes.data()+6)==48000,"single supported sample rate");
  req.wIndex=uint16_t(kFeature)<<8;req.wValue=uint16_t(AUDIO_FU_CTRL_VOLUME)<<8;
  check(tud_audio_get_req_entity_cb(0,&req) && replyBytes.size()==8 && int16_t(u16(replyBytes.data()+2))==-12800 && u16(replyBytes.data()+4)==0,"volume range -50..0 dB");
  req.bRequest=AUDIO_CS_REQ_CUR;req.wLength=2;int16_t level=-2560;
  check(tud_audio_set_req_entity_cb(0,&req,reinterpret_cast<uint8_t*>(&level)),"set master volume");
  check(tud_audio_get_req_entity_cb(0,&req) && int16_t(u16(replyBytes.data()))==level,"volume roundtrip");
  req.wValue|=1;check(!tud_audio_set_req_entity_cb(0,&req,reinterpret_cast<uint8_t*>(&level)),"unsupported channel rejected");
  req.wValue&=0xff00;req.wLength=1;check(!tud_audio_set_req_entity_cb(0,&req,reinterpret_cast<uint8_t*>(&level)),"short volume write rejected");
  req.wLength=2;level=256;check(!tud_audio_set_req_entity_cb(0,&req,reinterpret_cast<uint8_t*>(&level)),"out-of-range gain rejected");
  req.wValue=uint16_t(AUDIO_FU_CTRL_MUTE)<<8;req.wLength=1;uint8_t mute=1;
  check(tud_audio_set_req_entity_cb(0,&req,&mute) && hostMuted.load(),"mute set");
  check(tud_audio_get_req_entity_cb(0,&req) && replyBytes.size()==1 && replyBytes[0]==1,"mute read");
  puts("PASS: UAC2 stereo descriptors, CDC separation, feedback, clock/range, volume/mute, malformed controls");
 } catch(const std::exception& e) {fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
