import struct
with open("/app/tools/spd_dump/full-backup.bin", "rb") as f:
    data = f.read()

ptr = struct.pack("<I", 0x20021201)
idx = data.find(ptr)
while idx != -1:
    if idx % 4 == 0:
        print(f"Pointer to 0x20021201 at {hex(idx)}")
    idx = data.find(ptr, idx + 4)
