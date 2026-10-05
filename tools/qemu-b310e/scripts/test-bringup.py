#!/usr/bin/env python3
"""Exercise the flash and battery behavior required before stock audio init."""
import argparse
import hashlib
from pathlib import Path
import runpy

machine = runpy.run_path(str(Path(__file__).with_name("test-audio.py")))["machine"]
SFC = 0x20a00000


def command(qt, opcode, address=0, data=0, data_bytes=0):
    # Stock's software TX format: opcode, 24-bit address, native data word.
    qt.write(SFC + 8, 1)
    for slot in range(12):
        qt.write(SFC + 0x40 + slot * 4, 0)
    for slot in range(3):
        qt.write(SFC + 0x70 + slot * 4, 0)
    qt.write(SFC, 0)
    qt.write(SFC + 0x40, opcode)
    qt.write(SFC + 0x44, address)
    qt.write(SFC + 0x48, data)
    descriptor = 1 | (0x91 << 8)
    if data_bytes:
        descriptor |= (1 | ((data_bytes - 1) << 3)) << 16
    qt.write(SFC + 0x70, descriptor)
    qt.write(SFC + 4, 1)
    assert qt.read(SFC + 4) & 1 == 0
    assert qt.read(SFC + 0x10) & 1
    return qt.read(SFC + 0x5c)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.qemu = args.qemu.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.output / "test-nor.bin"
    fixture.write_bytes(b"\xff" * 0x800000)
    checksum = hashlib.sha256(fixture.read_bytes()).digest()
    with machine(args, "flash-battery", nor=fixture) as (qt, qmp, out):
        # Active-low DIGITAL EIC0, separately masked from analog END.
        eic = 0x8a001000
        assert qt.read(eic) == 0
        qt.write(eic + 4, 1)
        assert qt.read(eic) == 1
        qt.command("set_irq_in /machine/peripheral/sc6530-aux eic-input 0 0")
        qt.write(eic, 1)  # guest cannot override the physical level
        assert qt.read(eic) == 0
        assert int(qt.command(f"readb {eic:#x}")[0], 16) == 0
        qmp.command("system_reset")
        qt.write(eic + 4, 1)
        assert qt.read(eic) == 0  # external insertion survives reset
        qt.command("set_irq_in /machine/peripheral/sc6530-aux eic-input 0 1")
        assert qt.read(eic) == 1
        # External LCD status experiment obeys GPIO mask/direction and is
        # not a writable guest flag. Reset must not disconnect the wire.
        gpio = 0x8a000180
        qt.command("set_irq_in /machine/peripheral/sc6530-aux gpio-input 49 1")
        assert qt.read(gpio) == 0
        qt.write(gpio + 4, 2)
        assert qt.read(gpio) == 2
        qt.write(gpio + 8, 2)
        assert qt.read(gpio) == 0
        qt.write(gpio, 2)
        qt.command("set_irq_in /machine/peripheral/sc6530-aux gpio-input 49 0")
        assert qt.read(gpio) == 2
        qt.write(gpio + 8, 0)
        assert qt.read(gpio) == 0
        qt.command("set_irq_in /machine/peripheral/sc6530-aux gpio-input 49 1")
        qmp.command("system_reset")
        assert qt.read(gpio) == 0
        qt.write(gpio + 4, 2)
        assert qt.read(gpio) == 2
        qt.command("set_irq_in /machine/peripheral/sc6530-aux gpio-input 49 0")
        assert command(qt, 0x9f) == 0xc8601700
        assert command(qt, 0x05) == 0
        command(qt, 0x06)
        assert command(qt, 0x05) == 0x02000000
        # Match the stock driver's byte stores into a TX word, including +1.
        qt.write(SFC + 0x40, 0x02)
        qt.write(SFC + 0x44, 0x1010)
        qt.write(SFC + 0x48, 0)
        qt.command(f"writeb {SFC + 0x48:#x} 0xaa")
        qt.command(f"writeb {SFC + 0x49:#x} 0x55")
        assert qt.read(SFC + 0x48) == 0x55aa
        qt.write(SFC + 0x70, 0x00099101)
        # Writing a command buffer must not execute a transaction.
        assert qt.read(0x1010) == 0xffffffff
        qt.write(SFC + 4, 1)
        assert qt.read(0x1010) == 0xffff55aa
        assert command(qt, 0x05) == 0
        # Program needs WEL and only changes 1 bits to 0.
        command(qt, 0x02, 0x1010, 0, 2)
        assert qt.read(0x1010) == 0xffff55aa
        command(qt, 0x06)
        command(qt, 0x02, 0x1010, 0xfff0, 2)
        assert qt.read(0x1010) == 0xffff55a0
        command(qt, 0x06)
        command(qt, 0x20, 0x1fff)
        assert qt.read(0x1010) == 0xffffffff
        # A previous multiword program must not append stale payload slots
        # when the next transaction contains only a halfword. Leave the old
        # slot descriptors present, as the stock driver does.
        command(qt, 0x06)
        qt.write(SFC + 0x70, 0x1d0d9505)
        qt.write(SFC + 0x74, 0x1d1d1d1d)
        qt.write(SFC + 8, 1)
        assert qt.read(SFC + 0x70) & 0x01010101 == 0
        assert qt.read(SFC + 0x74) & 0x01010101 == 0
        assert qt.read(SFC + 8) & 1 == 0
        qt.write(SFC + 0x40, 0x02)
        qt.write(SFC + 0x44, 0x1010)
        qt.write(SFC + 0x48, 0x0101)
        qt.write(SFC + 0x70, 0x00099101)
        qt.write(SFC + 4, 1)
        assert qt.read(0x1010) == 0xffff0101
        qt.write(0x82001684, 0x35)
        qt.write(0x82001680, 3)
        assert qt.read(0x820016dc) & 1
        assert qt.read(0x820016cc) == 900
        qt.write(0x820016d4, 1)
        assert qt.read(0x820016dc) & 1 == 0
        # The ADI mailbox must resolve to the same converted result.
        qt.write(0x82000018, 0x6cc)
        assert qt.read(0x8200001c) & 0xffff == 900
        qt.write(0x80000008, 1 << 24)
        qt.write(0x82001588, 4)  # analog RTC source enable
        qt.write(0x82001630, 0xff00)
        qt.write(0x82001610, 58)
        qt.write(0x82001614, 59)
        qt.write(0x82001618, 23)
        qt.write(0x8200161c, 10)
        assert qt.read(0x82001634) & 0xf00 == 0xf00
        assert qt.read(0x82001580) == qt.read(0x82001584) == 4
        assert qt.read(0x80000000) == 1 << 24
        qt.write(0x82000018, 0x63c)
        assert qt.read(0x8200001c) & 0xffff == 0xf00
        qt.write(0x82001638, 0xffff)
        assert qt.read(0x80000000) == 0
        qt.advance(2_100_000_000)
        assert [qt.read(0x82001600 + i * 4) for i in range(4)] == [0, 0, 0, 11]
        qmp.command("system_reset")
        assert qt.read(0x820016dc) & 1 == 0
        assert qt.read(0x80000004) == 0
        qt.write(0x80000008, 1 << 8)
        qt.write(0x87000004, 0xff)
        qmp.command("cont")
        def key(down):
            qmp.command("input-send-event", {"events": [{"type": "key", "data": {
                "down": down, "key": {"type": "qcode", "data": "f2"}}}]})
        key(True)
        assert qt.read(0x87000008) == qt.read(0x8700000c) == 8
        assert qt.read(0x80000000) == 1 << 8
        qt.write(0x87000010, 1)  # clearing another slot preserves this one
        assert qt.read(0x80000000) == 1 << 8
        qt.write(0x87000010, 8)
        assert qt.read(0x80000000) == 0
        key(False)
        assert qt.read(0x87000008) == 0x80
        qt.write(0x87000010, 0xff)
        assert qt.read(0x80000004) == 0
    assert hashlib.sha256(fixture.read_bytes()).digest() == checksum
    print("PASS flash, immutable input, battery ADC, RTC, keypad IRQ/W1C")
    with machine(args, "interrupt-timer-dsp") as (qt, qmp, out):
        # Decode the IRQ through stock's masked-status register and preserve
        # existing enables with its read/OR/write pattern.
        qt.write(0x80000008, 1 << 13)
        qt.write(0x80000008, qt.read(0x80000008) | (1 << 23))
        assert qt.read(0x80000008) == (1 << 13) | (1 << 23)
        qt.write(0x10000000, 1)
        qt.write(0x8b000064, 0x10000)  # release DSP reset
        qt.write(0x10000004, (32 << 16) | 0x100)
        qt.write(0x10000000, 3)
        assert qt.read(0x10000004) == (32 << 16) | 0x100
        assert qt.read(0x10000000) >> 16 == 2
        qt.write(0x10000000, 5)
        assert qt.read(0x10000000) >> 16 == 4
        assert qt.read(0x8b000140) == 4
        assert qt.read(0x80000000) == 1 << 13
        qt.write(0x8000000c, 1 << 13)
        assert qt.read(0x80000000) == 0
        assert qt.read(0x80000004) == 1 << 13
        qt.write(0x8b000160, 8)  # frequency-request clear is not IRQ clear
        assert qt.read(0x80000004) == 1 << 13
        qt.write(0x8b000160, 4)
        assert qt.read(0x80000004) == 0
        # Timer0/1 run independently at 32768 Hz; stock dispatches IRQ 4/5.
        qt.write(0x80000008, (1 << 4) | (1 << 5))
        for base in [0x81000000, 0x81000020]:
            qt.write(base, 32768)
            qt.write(base + 12, 9)
            qt.write(base + 8, 0x80)
        qt.advance(500_000_000)
        assert 16382 <= qt.read(0x81000004) <= 16385
        assert qt.read(0x80000000) == 0
        for base in [0x81000000, 0x81000020]:
            before = qt.read(base + 4)
            qt.write(base + 8, 0)  # stock reads elapsed time after stopping
            assert qt.read(base + 4) == before
        frozen = qt.read(0x81000004)
        qt.advance(50_000_000)
        assert qt.read(0x81000004) == frozen
        for base in [0x81000000, 0x81000020]:
            qt.write(base + 8, 0x80)
        qt.advance(1_001_000_000)
        assert qt.read(0x80000000) == (1 << 4) | (1 << 5)
        for base in [0x81000000, 0x81000020]:
            qt.write(base + 8, 0)
            assert qt.read(base + 4) == 0
            qt.write(base + 12, 9)
        qt.advance(1_100_000_000)
        assert qt.read(0x80000004) == 0  # one-shot stays stopped
        # SYS_ALM compares the free-running millisecond count and reaches
        # the independent FIQ mask used by stock Syscnt_Init (IRQ id 16).
        qt.write(0x80000028, 1 << 16)
        qt.write(0x81003000, qt.read(0x8100300c) + 20)
        qt.write(0x81003008, 9)
        qt.advance(25_000_000)
        assert qt.read(0x80000020) == 1 << 16
        assert qt.read(0x80000000) == 0
        qt.write(0x81003008, 9)  # ACK keeps enabled, no stale retrigger
        qt.advance(100_000_000)
        assert qt.read(0x80000024) == 0
        qt.write(0x81003000, qt.read(0x8100300c) + 20)
        qt.advance(25_000_000)
        assert qt.read(0x80000020) == 1 << 16
        qt.write(0x8000002c, 1 << 16)
        assert qt.read(0x80000020) == 0
        assert qt.read(0x80000024) == 1 << 16
        qt.write(0x81003008, 8)
        assert qt.read(0x80000024) == 0
        qt.write(0x80000008, 1 << 14)
        qt.write(0x20d00110, 1)
        qt.write(0x20d00024, 0x34040000 >> 2)
        qt.write(0x20d00000, 8)
        assert qt.read(0x20d00118) == qt.read(0x20d0011c) == 1
        assert qt.read(0x80000000) == 1 << 14
        qt.write(0x20d00114, 1)
        assert qt.read(0x80000004) == 0
        # Display DMA retains pixels after the guest reuses its source.
        qt.memory(0x34040000, b"\x00\xf8" * (128 * 160))
        qt.write(0x20d00000, 8)
        assert qt.read(0x20d00000) & 8 == 0
        qt.memory(0x34040000, b"\0" * (128 * 160 * 2))
        qt.memory(0x34050000, b"\xe0\x07" * (128 * 160))
        qt.write(0x20d00024, 0x34050000 >> 2)
        qt.write(0x20d00000, qt.read(0x20d00000) | 1)
        image = out / "retained.ppm"
        qmp.command("screendump", {"filename": str(image), "format": "ppm"})
        assert image.read_bytes().endswith(b"\xf8\0\0" * (128 * 160))
        qt.write(0x20d00000, 8)
        qmp.command("screendump", {"filename": str(image), "format": "ppm"})
        assert image.read_bytes().endswith(b"\0\xfc\0" * (128 * 160))
        qt.write(0x20d00114, 1)
        # A one-shot must stop after expiry; clearing it must stay clear.
        qt.write(0x81000040, 26000)
        qt.write(0x8100004c, 9)
        qt.write(0x81000048, 0x80)
        qt.advance(2_000_000)
        assert qt.read(0x8100004c) == 7
        qt.write(0x8100004c, 9)
        qt.advance(2_000_000)
        assert qt.read(0x8100004c) == 1
        qt.write(0x2070002c, 0x07000001)
        assert qt.read(0x2070002c) == 3  # reset clears, internal clock stable
        assert qt.read(0x20700024) == 0  # no card
        qt.write(0x2070000c, 0x081a0000)
        assert qt.read(0x20700030) == 0x18000  # command timeout
        qt.write(0x20700030, 0x18000)
        assert qt.read(0x20700030) == 0
        qt.write(0x81000048, 0xc0)
        qt.advance(2_000_000)
        assert qt.read(0x8100004c) == 7
        qt.write(0x8100004c, 9)
        qt.advance(2_000_000)
        assert qt.read(0x8100004c) == 7
        qt.write(0x81000048, 0x40)  # mode bit alone must not enable it
        qt.write(0x8100004c, 9)
        qt.advance(2_000_000)
        assert qt.read(0x8100004c) == 1
        qmp.command("system_reset")
        assert qt.read(0x80000000) == qt.read(0x80000004) == 0
    print("PASS interrupt masks, DSP block/IRQ, timers, absent SD card, reset")


if __name__ == "__main__":
    main()
