import struct
with open("/app/tools/spd_dump/full-backup.bin", "rb") as f:
    data = f.read()

# Let's find "threadx_malloc.c"
pos = data.find(b"threadx_malloc.c")

# Find pointers to this string
ptr = struct.pack("<I", pos)
idx = data.find(ptr)
while idx != -1:
    if idx % 4 == 0:
        print(f"Pointer to threadx_malloc.c found at {hex(idx)}")
    idx = data.find(ptr, idx + 4)
