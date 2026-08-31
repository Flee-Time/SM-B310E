import struct
with open("/app/tools/spd_dump/full-backup.bin", "rb") as f:
    data = f.read()

ptr = struct.pack("<I", 0x0422c65c)
idx = data.find(ptr)
while idx != -1:
    if idx % 4 == 0:
        print(f"Pointer to 0x0422c65c at {hex(idx)}")
    idx = data.find(ptr, idx + 4)

ptr = struct.pack("<I", 0x0422c654)
idx = data.find(ptr)
while idx != -1:
    if idx % 4 == 0:
        print(f"Pointer to 0x0422c654 at {hex(idx)}")
    idx = data.find(ptr, idx + 4)
