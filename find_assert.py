with open("tools/qemu-b310e/logs/stock_real_exec.log", "r") as f:
    lines = []
    for line in f:
        lines.append(line.strip())
        if len(lines) > 500:
            lines.pop(0)
        if "val=0x00000041 pc=0x00038416" in line:
            for l in lines[-100:]:
                print(l)
            break
