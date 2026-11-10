import serial, struct, zlib, time
s = serial.Serial()
s.port = 'COM5'; s.baudrate = 115200; s.timeout = 0.3
s.dtr = False; s.rts = False
s.open()
s.dtr = False; s.rts = False
time.sleep(0.3)
s.rts = True          # EN low
time.sleep(0.2)
s.rts = False         # EN high, IO0 stays high -> boot app
time.sleep(3.0)
boot = s.read(65536)
payload = b'{"version":4,"cmd":"sys","act":"ping"}'
hdr = bytearray(16); hdr[0:4] = b'SBX4'
hdr[4:6] = struct.pack('<H', len(payload)); hdr[6:8] = struct.pack('<H', 0)
hdr[8:12] = struct.pack('<I', zlib.crc32(bytes(hdr[0:8])) & 0xffffffff)
hdr[12:16] = struct.pack('<I', zlib.crc32(payload) & 0xffffffff)
s.write(bytes(hdr) + payload)
time.sleep(1.5)
reply = s.read(65536)
s.rts = False
s.close()
bt = boot.decode('utf-8', 'replace')
print('BOOT BYTES', len(boot))
print(bt[:1200])
print('---')
rt = reply.decode('utf-8', 'replace')
print('REPLY BYTES', len(reply), 'PONG' if 'pong' in rt else 'NO PONG')
print(rt[:400])
