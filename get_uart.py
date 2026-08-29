chars = []
with open("tools/qemu-b310e/logs/stock_real_fast.log", "r") as f:
    for line in f:
        if "addr=0x84000000 val=" in line:
            parts = line.split("val=")
            if len(parts) > 1:
                try:
                    val = int(parts[1][:10], 16)
                    if val < 256 and val != 0:
                        chars.append(chr(val))
                except:
                    pass
print("".join(chars))
