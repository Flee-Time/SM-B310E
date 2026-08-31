import struct
with open("/app/tools/spd_dump/full-backup.bin", "rb") as f:
    data = f.read()

pos = data.find(b"AST_BLUESCREEN")
print(f"AST_BLUESCREEN at {hex(pos)}")

ptr = struct.pack("<I", pos)
idx = data.find(ptr)
while idx != -1:
    if idx % 4 == 0:
        print(f"Pointer to AST_BLUESCREEN at {hex(idx)}")
    idx = data.find(ptr, idx + 4)
