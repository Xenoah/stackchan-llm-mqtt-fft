# Adapted from Xenoah/cores3-usb-spectrum v1.1.0 (MIT).
# See docs/licenses/cores3-usb-spectrum.txt.
"""End-to-end test using Windows' built-in waveOut API, addressed to this device.
Does not change the system default output or install a runtime/driver.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import math
from pathlib import Path
import struct
import time
import urllib.request
import urllib.parse

class Caps(C.Structure):
    _fields_=[('mid',W.WORD),('pid',W.WORD),('version',W.DWORD),
              ('name',W.WCHAR*32),('formats',W.DWORD),('channels',W.WORD),
              ('reserved',W.WORD),('support',W.DWORD)]
class Format(C.Structure):
    _pack_=2
    _fields_=[('tag',W.WORD),('channels',W.WORD),('rate',W.DWORD),
              ('bytes',W.DWORD),('align',W.WORD),('bits',W.WORD),('extra',W.WORD)]
class Header(C.Structure):
    _fields_=[('data',C.c_void_p),('length',W.DWORD),('recorded',W.DWORD),
              ('user',C.c_size_t),('flags',W.DWORD),('loops',W.DWORD),
              ('next',C.c_void_p),('reserved',C.c_size_t)]
winmm=C.WinDLL('winmm')
winmm.waveOutGetNumDevs.restype=W.UINT
winmm.waveOutGetDevCapsW.argtypes=[C.c_size_t,C.POINTER(Caps),W.UINT]
winmm.waveOutOpen.argtypes=[C.POINTER(C.c_void_p),W.UINT,C.POINTER(Format),C.c_size_t,C.c_size_t,W.DWORD]
for fn in ['waveOutPrepareHeader','waveOutWrite','waveOutUnprepareHeader']:
    getattr(winmm,fn).argtypes=[C.c_void_p,C.POINTER(Header),W.UINT]
winmm.waveOutReset.argtypes=[C.c_void_p]
winmm.waveOutClose.argtypes=[C.c_void_p]

def checked(code):
    if code: raise RuntimeError(f'Windows waveOut error {code}')

def devices():
    result=[]
    for index in range(winmm.waveOutGetNumDevs()):
        caps=Caps(); checked(winmm.waveOutGetDevCapsW(index,C.byref(caps),C.sizeof(caps)))
        result.append((index,caps.name))
    return result

class Playback:
    def __init__(self,index,hz,seconds=2,opposite=False,multitone=False,motion=False):
        self.handle=C.c_void_p()
        fmt=Format(1,2,48000,48000*4,4,16,0)
        checked(winmm.waveOutOpen(C.byref(self.handle),index,C.byref(fmt),0,0,0))
        pcm=bytearray(int(48000*seconds)*4)
        for i in range(len(pcm)//4):
            if motion:
                t=i/48000
                level=.15+.85*(.5+.5*math.sin(2*math.pi*2.3*t))
                l=int(10000*level*math.sin(2*math.pi*(120*t+270*t*t)))
                r=int(10000*level*math.sin(2*math.pi*(600*t+80*t*t)))
            elif multitone:
                a=sum(math.sin(2*math.pi*f*i/48000)*w for f,w in [(65,.2),(180,.15),(440,.2),(1000,.15),(3200,.15),(9600,.1)])
                b=sum(math.sin(2*math.pi*f*i/48000)*w for f,w in [(90,.2),(260,.15),(660,.2),(1800,.15),(5200,.1)])
                l=int(8192*a); r=int(8192*b)
            else:
                l=int(4096*math.sin(2*math.pi*hz*i/48000)); r=-l if opposite else l
            struct.pack_into('<hh',pcm,i*4,l,r)
        self.data=C.create_string_buffer(bytes(pcm))
        self.header=Header(C.cast(self.data,C.c_void_p),len(pcm),0,0,0,0,None,0)
        checked(winmm.waveOutPrepareHeader(self.handle,C.byref(self.header),C.sizeof(Header)))
        checked(winmm.waveOutWrite(self.handle,C.byref(self.header),C.sizeof(Header)))
    def close(self):
        checked(winmm.waveOutReset(self.handle))
        checked(winmm.waveOutUnprepareHeader(self.handle,C.byref(self.header),C.sizeof(Header)))
        checked(winmm.waveOutClose(self.handle))


def get(base, path='/api/usb-spectrum'):
    with urllib.request.urlopen(base+path,timeout=8) as response:
        return json.load(response)

def set_mode(base,mode):
    data=urllib.parse.urlencode({'mode':mode}).encode()
    with urllib.request.urlopen(base+'/api/mode',data=data,timeout=8) as response:
        assert json.load(response)['ok']

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url',required=True,help='Device URL, e.g. http://192.168.0.190')
    parser.add_argument('--output',default='.pio/spectrum-test/device-validation.json')
    parser.add_argument('--soak',type=int,default=0,help='Optional continuous silent PCM input, seconds')
    args=parser.parse_args()
    base=args.url.rstrip('/')
    available=devices()
    found=[i for i,name in available if 'StackChan USB Spectru' in name]
    if len(found)!=1: raise RuntimeError(f'Expected one StackChan endpoint, found {available}')
    previous=get(base,'/api/status')['mode']
    results=[]
    try:
        set_mode(base,'llm')
        time.sleep(.4)
        # Reopen the real endpoint for each tone; this catches missing close/
        # restart support as well as a host that silently plays into a failed pipe.
        for hz,opposite in [(440,False),(1000,False),(4000,False),(1000,True),(0,False)]:
            before=get(base)
            play=Playback(found[0],hz,seconds=12,opposite=opposite)
            try:
                time.sleep(1.2)
                readings=[]
                for _ in range(3):
                    readings.append(get(base));time.sleep(.3)
                last=readings[-1]
                result={'tone_hz':hz,'opposite_phase':opposite,'before':before,'readings':readings}
                result['passed']=all(v['active'] and v['alternate']==1 for v in readings)
                result['passed'] &= last['frames']>before['frames'] and last['analyses']>before['analyses']
                result['passed'] &= last['speaker_bytes']>before['speaker_bytes']
                result['passed'] &= last['control_failures']==before['control_failures']
                result['passed'] &= all(abs(v['peak_hz']-hz)<5 for v in readings) if hz else all(v['left_dbfs']<=-100 and v['peak_hz']==0 for v in readings)
                results.append(result)
                print(json.dumps({'tone_hz':hz,'opposite_phase':opposite,'passed':result['passed'],'received_bytes':last['received_bytes'],'speaker_bytes':last['speaker_bytes'],'peak_hz':last['peak_hz'],'dropped_bytes':last['dropped_bytes']}),flush=True)
            finally: play.close()
            time.sleep(.5)
        if args.soak:
            before=get(base)
            play=Playback(found[0],0,seconds=args.soak+5)
            try:
                readings=[]
                deadline=time.monotonic()+args.soak
                while time.monotonic()<deadline:
                    time.sleep(2);v=get(base);readings.append(v)
                    print('soak',v['received_bytes'],v['speaker_bytes'],v['peak_hz'],v['dropped_bytes'],flush=True)
                result={'soak_tone_hz':0,'soak_seconds':args.soak,'before':before,'readings':readings}
                result['passed']=all(v['active'] and v['peak_hz']==0 for v in readings)
                result['passed'] &= all(v['dropped_bytes']==before['dropped_bytes'] and v['control_failures']==before['control_failures'] for v in readings)
                result['passed'] &= all(b['received_bytes']>a['received_bytes'] and b['speaker_bytes']>a['speaker_bytes'] for a,b in zip(readings,readings[1:]))
                results.append(result)
            finally: play.close()
        time.sleep(.5)
        idle=get(base)
        results.append({'idle':idle,'passed':not idle['active'] and idle['peak_hz']==0})
    finally:
        set_mode(base,{'MQTT':'mqtt','LOCAL LLM':'llm','LEVEL HOLD':'level'}.get(previous,'llm'))
        output=Path(args.output);output.parent.mkdir(parents=True,exist_ok=True)
        output.write_text(json.dumps(results,indent=2),encoding='utf-8')
    if not all(r['passed'] for r in results): raise SystemExit('DEVICE USB AUDIO/FFT TEST FAILED')
    print('PASS: device PCM reception, speaker queue, FFT frequencies, stream reopen and idle',flush=True)
