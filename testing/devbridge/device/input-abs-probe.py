import os,fcntl,struct
for path in ('/dev/input/event1','/dev/input/event3'):
 fd=os.open(path,os.O_RDONLY|os.O_NONBLOCK)
 for axis in (16,17):
  data=bytearray(24)
  fcntl.ioctl(fd,0x80184540+axis,data,True)
  print(path,'axis',axis,'value/min/max/fuzz/flat/resolution',struct.unpack('6i',data),flush=True)
 os.close(fd)
