import struct
with open("/app/tools/spd_dump/full-backup.bin", "rb") as f:
    data = f.read()

ptr = struct.pack("<I", 0x04259620)
idx = data.find(ptr)
while idx != -1:
    if idx % 4 == 0:
        print(f"Pointer to 0x04259620 at {hex(idx)}")
    idx = data.find(ptr, idx + 4)
