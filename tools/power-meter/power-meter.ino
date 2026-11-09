/* B310E power meter: an Arduino Leonardo reads the current in a USB cable's ground wire.
 *
 * The phone runs from the cable with no battery; a small resistor (about 0.3 to 1 ohm)
 * sits in the cable's ground wire, and red and the data wires are untouched:
 *
 *     PC side of the resistor     to Leonardo GND and A4 (ADC1)
 *     phone side of the resistor  to Leonardo A5 (ADC0) and A3 (ADC4)
 *
 * The ATmega32U4's differential ADC has gains on a few fixed pairs of inputs. The result
 * is signed, so polarity does not matter (the PC side takes the sign off). Code 9 at 10x
 * is what is used: 40x and 200x saturate on the pulses described in loop(). Code 8 is the
 * same at zero. "scan" and "raw" are for looking; the choice lives in EEPROM ("range").
 *
 * Over the USB serial port, 115200 (the rate is ignored on this chip's USB):
 *     scan                 one line per code: mux, mean counts, min, max
 *     range N MUX GAIN     range N (0 = most sensitive): this code, this gain
 *     ranges               what is set
 *     raw MUX QUIET        300 samples in a row, interrupts off if QUIET
 *     go / stop            lines about every 100 ms: ms,range,mid-mean,min,max,samples (counts)
 * The PC side turns counts into mA: mA = counts * 2560 / (512 * gain * ohms).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <EEPROM.h>

#define MAX_RANGES 4
#define WINDOW 512
#define EE_MAGIC 0xB3

struct range_t {
    uint8_t mux, gain;
};

static range_t ranges[MAX_RANGES];
static uint8_t n_ranges, current;
static bool streaming;
static char line[40];
static uint8_t line_len;

static void select_mux(uint8_t mux)
{
    /* 2.56 V reference; low 5 bits in ADMUX, the 6th is MUX5 in ADCSRB */
    ADMUX = _BV(REFS1) | _BV(REFS0) | (mux & 0x1f);
    if (mux & 0x20)
        ADCSRB |= _BV(MUX5);
    else
        ADCSRB &= ~_BV(MUX5);
}

/* Differential results are 10-bit two's complement; single-ended are 0..1023 and only
 * used by the scan, where the PC side ignores them. */
static int16_t convert(void)
{
    ADCSRA |= _BV(ADSC);
    while (ADCSRA & _BV(ADSC))
        ;
    uint16_t v = ADC;
    return (v & 0x200) ? (int16_t)(v | 0xfc00) : (int16_t)v;
}

static void settle(void)
{
    /* the first conversions after a mux change or gain change are off */
    for (uint8_t i = 0; i < 4; i++)
        convert();
}

static void load_ranges(void)
{
    n_ranges = 0;
    if (EEPROM.read(0) != EE_MAGIC)
        return;
    uint8_t n = EEPROM.read(1);
    if (n > MAX_RANGES)
        return;
    for (uint8_t i = 0; i < n; i++) {
        ranges[i].mux = EEPROM.read(2 + 2 * i);
        ranges[i].gain = EEPROM.read(3 + 2 * i);
    }
    n_ranges = n;
}

static void save_ranges(void)
{
    EEPROM.update(0, EE_MAGIC);
    EEPROM.update(1, n_ranges);
    for (uint8_t i = 0; i < n_ranges; i++) {
        EEPROM.update(2 + 2 * i, ranges[i].mux);
        EEPROM.update(3 + 2 * i, ranges[i].gain);
    }
}

static void scan(void)
{
    for (uint8_t mux = 0; mux < 64; mux++) {
        select_mux(mux);
        settle();
        long sum = 0;
        int16_t lo = 32767, hi = -32768;
        for (int i = 0; i < 256; i++) {
            int16_t v = convert();
            sum += v;
            if (v < lo)
                lo = v;
            if (v > hi)
                hi = v;
        }
        Serial.print(F("scan,"));
        Serial.print(mux);
        Serial.print(',');
        Serial.print(sum / 256);
        Serial.print(',');
        Serial.print(lo);
        Serial.print(',');
        Serial.println(hi);
    }
    Serial.println(F("scan,done"));
}

/* Consecutive samples of one code with interrupts off or on, to see what the signal does
 * (the USB interrupts run during a normal reading). */
static void raw(uint8_t mux, bool quiet)
{
    static int16_t samples[300];

    select_mux(mux);
    settle();
    if (quiet)
        noInterrupts();
    uint32_t t0 = micros();
    for (uint16_t i = 0; i < 300; i++)
        samples[i] = convert();
    uint32_t t1 = micros();
    interrupts();
    Serial.print(F("raw,"));
    Serial.print(mux);
    Serial.print(',');
    Serial.print(quiet);
    Serial.print(',');
    Serial.println(t1 - t0);
    for (uint16_t i = 0; i < 300; i++) {
        Serial.print(samples[i]);
        if (i % 30 == 29)
            Serial.println();
        else
            Serial.print(' ');
    }
    Serial.println(F("raw,done"));
}

static void command(char *s)
{
    int a, b, c;

    if (sscanf(s, "raw %d %d", &a, &b) == 2 && a >= 0 && a < 64) {
        streaming = false;
        raw((uint8_t)a, b != 0);
    } else if (!strcmp(s, "scan")) {
        streaming = false;
        scan();
    } else if (!strcmp(s, "go")) {
        if (!n_ranges) {
            Serial.println(F("err,no ranges: meter.py --scan, then --range"));
            return;
        }
        current = 0;
        select_mux(ranges[0].mux);
        settle();
        streaming = true;
    } else if (!strcmp(s, "stop")) {
        streaming = false;
    } else if (!strcmp(s, "ranges")) {
        for (uint8_t i = 0; i < n_ranges; i++) {
            Serial.print(F("range,"));
            Serial.print(i);
            Serial.print(',');
            Serial.print(ranges[i].mux);
            Serial.print(',');
            Serial.println(ranges[i].gain);
        }
        Serial.println(F("ranges,done"));
    } else if (sscanf(s, "range %d %d %d", &a, &b, &c) == 3 && a >= 0 && a < MAX_RANGES &&
               b >= 0 && b < 64 && c > 0 && c <= 200) {
        ranges[a].mux = (uint8_t)b;
        ranges[a].gain = (uint8_t)c;
        n_ranges = (uint8_t)a + 1;
        save_ranges();
        Serial.println(F("ok"));
    } else {
        Serial.println(F("err,scan | range N MUX GAIN | ranges | go | stop"));
    }
}

void setup()
{
    Serial.begin(115200);
    /* ADC clock 125 kHz: the slowest, for the gain stages */
    ADCSRA = _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);
    DIDR0 = _BV(ADC0D) | _BV(ADC1D) | _BV(ADC4D);
    load_ranges();
}

void loop()
{
    while (Serial.available()) {
        char c = (char)Serial.read();

        if (c == '\n' || c == '\r') {
            line[line_len] = 0;
            if (line_len)
                command(line);
            line_len = 0;
        } else if (line_len + 1 < sizeof(line)) {
            line[line_len++] = c;
        }
    }
    if (!streaming)
        return;

    /* The signal is a small steady voltage with sharp pulses on it about every 0.4 ms, up
     * to hundreds of mV, whatever the phone is doing (phone, 2026-11-09; a sample in five or
     * so). The ADC's zero channel does not see them, so they are at the inputs, and an
     * average, even a multimeter's, is mostly them. The mean of the middle half of the
     * samples leaves them out: it told chip sleep on from off, repeatably, when no plain
     * average did. It is the steady part of the current, not any fast pulses of it. */
    static int16_t w[WINDOW];
    int16_t lo = 32767, hi = -32768;

    for (uint16_t i = 0; i < WINDOW; i++) {
        int16_t v = convert();
        int16_t k = (int16_t)i - 1;

        if (v < lo)
            lo = v;
        if (v > hi)
            hi = v;
        /* in order as they come: little more than a conversion's time each */
        while (k >= 0 && w[k] > v) {
            w[k + 1] = w[k];
            k--;
        }
        w[k + 1] = v;
    }
    long sum = 0;

    for (uint16_t i = WINDOW / 4; i < 3 * WINDOW / 4; i++)
        sum += w[i];
    Serial.print(millis());
    Serial.print(',');
    Serial.print(current);
    Serial.print(',');
    Serial.print((float)sum / (WINDOW / 2), 2);
    Serial.print(',');
    Serial.print(lo);
    Serial.print(',');
    Serial.print(hi);
    Serial.print(',');
    Serial.println(WINDOW);
}
