import struct
with open("/app/tools/spd_dump/full-backup.bin", "rb") as f:
    data = f.read()

for offset in range(-64, 64, 4):
    ptr = struct.pack("<I", 0x0422e554 + offset)
    idx = data.find(ptr)
    if idx != -1 and idx % 4 == 0:
        print(f"Pointer to {hex(0x0422e554 + offset)} at {hex(idx)}")
