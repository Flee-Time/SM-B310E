import struct
with open("/app/tools/spd_dump/full-backup.bin", "rb") as f:
    data = f.read()

# Let's search around 0x0423c800
for offset in range(-64, 64, 4):
    ptr = struct.pack("<I", 0x0423c818 + offset)
    idx = data.find(ptr)
    if idx != -1 and idx % 4 == 0:
        print(f"Pointer to {hex(0x0423c818 + offset)} at {hex(idx)}")
