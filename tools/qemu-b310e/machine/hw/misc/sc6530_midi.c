/*
 * SC6530C wavetable accelerator, 0x20b00000.
 * Register ABI: stock NOR 0xb2e38..0xb30c0 and 0xc0088..0xc01f8.
 * Voice ABI: stock 0xad9f4..0xadb6c; corroborated by SCI_CTRLBLK debug types.
 * Independently implemented interpolation, envelopes and mixing. Public
 * Sonivox EAS wavetable documentation/source corroborates Q15 pitch/gain
 * conventions (see docs/emulator-audio.md). No vendor code or tables here.
 * Filter coefficients and exact hardware rounding remain unverified.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "trace.h"
#include <math.h>

#define TYPE_SC6530_MIDI "sc6530_midi"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530MidiState, SC6530_MIDI)

struct Sc6530MidiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t reg[10];
    bool pending;
};

/* DMA must resolve to actual memory, never to the fallback MMIO bank. */
static bool midi_memory(uint32_t addr, size_t len, bool write)
{
    MemoryRegionSection sec = memory_region_find(get_system_memory(), addr, len);
    bool valid = sec.mr && (memory_region_is_ram(sec.mr) ||
                           (!write && memory_region_is_romd(sec.mr))) &&
                 int128_get64(sec.size) == len &&
                 (!write || !memory_region_is_rom(sec.mr));
    if (sec.mr) {
        memory_region_unref(sec.mr);
    }
    return valid;
}

static int32_t q15(int32_t a, int32_t b)
{
    return ((int64_t)a * b) >> 15;
}

static int32_t voice_s16(const uint8_t *voice, unsigned offset)
{
    return (int16_t)lduw_le_p(voice + offset);
}

/* EG1 is exponential after attack; EG2 decays linearly in pitch units. */
static void midi_envelope(uint8_t *v, unsigned which)
{
    unsigned state_off = 0x1c + which;
    unsigned value_off = 0x20 + which * 4;
    unsigned art_off = 0x50 + which * 4;
    int32_t value = voice_s16(v, value_off);
    int32_t increment = voice_s16(v, value_off + 2);
    int32_t sustain = voice_s16(v, art_off + 2);
    switch (v[state_off]) {
    case 2: /* attack */
        value += increment;
        if (value >= 32767) {
            value = 32767;
            v[state_off] = 4;
            stw_le_p(v + value_off + 2, lduw_le_p(v + art_off));
        }
        break;
    case 4: /* decay */
        value = which ? value - increment : q15(value, increment);
        if (value <= sustain) {
            value = sustain;
            v[state_off] = !which && !value ? 8 : 5;
        }
        break;
    case 5: /* sustain */
        return;
    case 6: /* release */
        value = which ? value - increment : q15(value, increment);
        if (value <= 0) {
            value = 0;
            v[state_off] = 8;
        }
        break;
    default:
        value = 0;
        break;
    }
    stw_le_p(v + value_off, CLAMP(value, 0, 32767));
}

static bool midi_voice(uint8_t *v, int64_t mix[256][2], unsigned frames,
                       bool sample16, bool stereo)
{
    uint32_t pos = ldl_le_p(v + 0x30), frac = ldl_le_p(v + 0x34) & 0x7fff;
    uint32_t start = ldl_le_p(v + 0x28), end = ldl_le_p(v + 0x2c);
    unsigned width = sample16 ? 2 : 1;
    bool noise = start == UINT32_MAX, done = false;
    int32_t pitch, target, previous = voice_s16(v, 0x10);
    int32_t lfo_phase = voice_s16(v, 0x3c), lfo = voice_s16(v, 0x3e);
    uint32_t step;

    v[0] = 0;
    if (!v[0xf]) { /* inactive sentinel used when no voices are playing */
        return true;
    }
    midi_envelope(v, 0);
    midi_envelope(v, 1);
    if (lfo_phase < 0) {
        lfo_phase++;
    } else {
        lfo = (int16_t)(lfo_phase * 4);
        if (lfo_phase > 0x1fff && lfo_phase < 0x6000) {
            lfo = (int16_t)~lfo;
        }
        lfo_phase = (lfo_phase + voice_s16(v, 0x48)) & 0x7fff;
    }
    stw_le_p(v + 0x3c, lfo_phase);
    stw_le_p(v + 0x3e, lfo);
    pitch = (int32_t)ldl_le_p(v + 0x14) + voice_s16(v, 0xa) + v[0xc] * 100;
    if (!(v[0x18] & 8)) {
        pitch += (int8_t)v[0x1b] * 100;
    }
    pitch += q15(voice_s16(v, 0x24), voice_s16(v, 0x4c));
    pitch += q15(lfo, voice_s16(v, 0x4a) + (v[0x19] + v[0x1a]) * 50 / 128);
    /* 1200 cents is one octave; phase has 15 fractional bits. */
    step = (uint32_t)(32768.0 * exp2(CLAMP(pitch, -18000, 18000) / 1200.0));
    target = q15(v[0xd] * 256, v[0xd] * 256);
    target = q15(target, voice_s16(v, 8));
    target = q15(target, voice_s16(v, 0x12));
    target = q15(target, voice_s16(v, 0x20));
    target = (int32_t)CLAMP(target * exp2(q15(lfo, (int8_t)v[0x47]) / 20.0 *
                                        log2(10.0)), 0.0, 32767.0);
    if (!noise && (end < pos || !midi_memory(pos, (uint64_t)end - pos + width, false))) {
        return false;
    }
    if (!noise && start != end && (start > end || !midi_memory(start, (uint64_t)end - start + width, false))) {
        return false;
    }
    for (unsigned i = 0; i < frames; i++) {
        int32_t a, b, sample, gain;
        if (done) {
            break;
        }
        if (noise) {
            a = (int32_t)pos >> 18;
            b = (int32_t)end >> 18;
        } else {
            uint8_t bytes[4] = { 0 };
            uint32_t next = pos + width;
            if (next > end) {
                next = start == end ? pos : start;
            }
            if (address_space_read(&address_space_memory, pos, MEMTXATTRS_UNSPECIFIED,
                                   bytes, width) != MEMTX_OK ||
                address_space_read(&address_space_memory, next, MEMTXATTRS_UNSPECIFIED,
                                   bytes + 2, width) != MEMTX_OK) {
                return false;
            }
            a = sample16 ? (int16_t)lduw_le_p(bytes) : (int8_t)bytes[0] * 256;
            b = sample16 ? (int16_t)lduw_le_p(bytes + 2) : (int8_t)bytes[2] * 256;
            a >>= 2;
            b >>= 2;
        }
        sample = a + (((int64_t)(b - a) * frac) >> 15);
        gain = previous + ((int64_t)(target - previous) * (i + 1)) / frames;
        /* Q15 gains, two interpolation headroom bits, four mixer guard bits. */
        if (stereo) {
            int32_t scaled = ((int64_t)sample * gain) >> 14;
            mix[i][0] += ((int64_t)scaled * voice_s16(v, 0x3a)) >> 4;
            mix[i][1] += ((int64_t)scaled * voice_s16(v, 0x38)) >> 4;
        } else {
            mix[i][0] += ((int64_t)sample * gain) >> 3;
        }
        uint64_t phase = (uint64_t)frac + step;
        frac = phase & 0x7fff;
        if (noise) {
            if (phase >> 15) {
                pos = end;
                end = end * 5u + 1u;
            }
        } else {
            uint64_t advance = (uint64_t)pos + (phase >> 15) * width;
            if (advance > end) {
                if (start == end) {
                    pos = end;
                    done = true;
                } else {
                    pos = start + (advance - end - width) % ((uint64_t)end - start + width);
                }
            } else {
                pos = advance;
            }
        }
    }
    stl_le_p(v + 0x30, pos);
    stl_le_p(v + 0x34, frac);
    if (noise) {
        stl_le_p(v + 0x2c, end);
    }
    stw_le_p(v + 0x10, target);
    v[0xe] &= ~8;
    v[0] = done || (v[0xf] != 5 && v[0x1c] == 8);
    return true;
}

static void midi_irq(Sc6530MidiState *s)
{
    qemu_set_irq(s->irq, s->pending && (s->reg[2] & 1));
}

static void midi_run(Sc6530MidiState *s)
{
    int64_t mix[256][2] = { 0 };
    uint8_t output[2048];
    uint32_t addr = s->reg[4], seen[64];
    unsigned voices = 0, frames = 32u << ((s->reg[1] >> 8) & 3);
    unsigned limit = 1 + ((s->reg[1] >> 16) & 63);
    bool stereo = s->reg[1] & 1, sample16 = s->reg[1] & 2, error = false;
    unsigned channels = stereo ? 2 : 1, bytes = frames * channels * 4;
    if (!midi_memory(s->reg[5], bytes, true)) {
        error = true;
        goto finish;
    }
    while (addr != UINT32_MAX && addr && voices < limit) {
        uint8_t v[96];
        for (unsigned i = 0; i < voices; i++) {
            if (seen[i] == addr) {
                error = true;
                goto finish;
            }
        }
        if (!midi_memory(addr, sizeof(v), true) ||
            address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                               v, sizeof(v)) != MEMTX_OK ||
            !midi_voice(v, mix, frames, sample16, stereo)) {
            error = true;
            goto finish;
        }
        address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, v, sizeof(v));
        seen[voices++] = addr;
        addr = ldl_le_p(v + 4);
    }
    for (unsigned i = 0; i < frames; i++) {
        for (unsigned c = 0; c < channels; c++) {
            stl_le_p(output + (i * channels + c) * 4,
                     CLAMP(mix[i][c], INT32_MIN, INT32_MAX));
        }
    }
    address_space_write(&address_space_memory, s->reg[5], MEMTXATTRS_UNSPECIFIED, output, bytes);
finish:
    s->reg[0] &= ~1u;
    s->reg[3] = voices & 0x7f;
    s->pending = true;
    trace_sc6530_midi_render(s->reg[4], s->reg[5], frames, voices, error);
    if (error) {
        qemu_log_mask(LOG_GUEST_ERROR, "sc6530_midi: invalid voice/sample/output memory\n");
    }
    midi_irq(s);
}

static uint64_t midi_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530MidiState *s = opaque;
    unsigned index = offset / 4;
    uint32_t value = index < ARRAY_SIZE(s->reg) ? s->reg[index] : 0;
    if (index == 2) {
        value = (s->reg[2] & 1) | (s->pending ? 1u << 24 : 0) |
                (s->pending && (s->reg[2] & 1) ? 1u << 16 : 0);
    }
    return value;
}

static void midi_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Sc6530MidiState *s = opaque;
    unsigned index = offset / 4;
    if (index >= ARRAY_SIZE(s->reg)) {
        return;
    }
    if (index == 2) {
        s->pending &= !(value & 0x100);
        s->reg[2] = value & 1;
        midi_irq(s);
    } else if (index != 3) {
        s->reg[index] = value;
        if (index == 0 && (value & 1)) {
            midi_run(s);
        }
    }
}

static const MemoryRegionOps midi_ops = {
    .read = midi_read, .write = midi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void midi_reset(DeviceState *dev)
{
    Sc6530MidiState *s = SC6530_MIDI(dev);
    memset(s->reg, 0, sizeof(s->reg));
    s->pending = false;
    midi_irq(s);
}

static int midi_post_load(void *opaque, int version)
{
    midi_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_midi = {
    .name = TYPE_SC6530_MIDI, .version_id = 1, .minimum_version_id = 1,
    .post_load = midi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, Sc6530MidiState, 10),
        VMSTATE_BOOL(pending, Sc6530MidiState), VMSTATE_END_OF_LIST()
    },
};

static void midi_init(Object *obj)
{
    Sc6530MidiState *s = SC6530_MIDI(obj);
    memory_region_init_io(&s->iomem, obj, &midi_ops, s, "sc6530-midi", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void midi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, midi_reset);
    dc->vmsd = &vmstate_midi;
}

static const TypeInfo midi_info = {
    .name = TYPE_SC6530_MIDI, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530MidiState), .instance_init = midi_init,
    .class_init = midi_class_init,
};

static void midi_register_types(void)
{
    type_register_static(&midi_info);
}
type_init(midi_register_types)
