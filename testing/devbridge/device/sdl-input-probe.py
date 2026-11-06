"""No-window SDL inventory for DS input diagnostics."""
import ctypes as c
import os,sys,time,struct
s = c.CDLL('/mnt/SDCARD/App/PyUI/dll-mali/libSDL2-2.0.so.0')
s.SDL_JoystickNameForIndex.restype = c.c_char_p
s.SDL_JoystickOpen.restype = c.c_void_p
for f in (s.SDL_JoystickNumButtons, s.SDL_JoystickNumHats, s.SDL_JoystickNumAxes):
    f.argtypes = [c.c_void_p]
class Guid(c.Structure):
    _fields_ = [('data', c.c_ubyte * 16)]
s.SDL_JoystickGetDeviceGUID.restype = Guid
s.SDL_SetHint(b'SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS', b'1')
pads=[]
print('before SDL init', flush=True)
print('SDL init:', s.SDL_Init(0x2200), flush=True)
print('joysticks:', s.SDL_NumJoysticks(), flush=True)
for i in range(s.SDL_NumJoysticks()):
    print('opening', i, s.SDL_JoystickNameForIndex(i), bytes(s.SDL_JoystickGetDeviceGUID(i).data).hex(), flush=True)
    j = s.SDL_JoystickOpen(i)
    pads.append(j)
    print('controls:', s.SDL_JoystickNumButtons(j), 'buttons', s.SDL_JoystickNumHats(j), 'hats', s.SDL_JoystickNumAxes(j), 'axes; mapped:', s.SDL_IsGameController(i), flush=True)
if '--probe' in sys.argv:
    s.SDL_JoystickGetHat.argtypes=[c.c_void_p,c.c_int]
    s.SDL_JoystickGetHat.restype=c.c_uint8
    s.SDL_JoystickGetButton.argtypes=[c.c_void_p,c.c_int]
    s.SDL_JoystickGetButton.restype=c.c_uint8
    old={}
    end=time.monotonic()+8
    while time.monotonic()<end:
        s.SDL_JoystickUpdate()
        for i,j in enumerate(pads):
            state=(s.SDL_JoystickGetHat(j,0),tuple(k for k in range(s.SDL_JoystickNumButtons(j)) if s.SDL_JoystickGetButton(j,k)))
            if old.get(i)!=state:
                print('state',i,state,flush=True);old[i]=state
        time.sleep(.01)
s.SDL_Quit()
