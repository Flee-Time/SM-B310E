import struct
with open("/app/tools/spd_dump/full-backup.bin", "rb") as f:
    data = f.read()

for p in [0x0423c818, 0x0422c654]:
    ptr = struct.pack("<I", p)
    idx = data.find(ptr)
    while idx != -1:
        if idx % 4 == 0:
            print(f"Pointer to {hex(p)} at {hex(idx)}")
        idx = data.find(ptr, idx + 4)
