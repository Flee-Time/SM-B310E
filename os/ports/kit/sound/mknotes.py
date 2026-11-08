#!/usr/bin/env python3
"""The synth's note steps and sine (b310e-notes.h).
SPDX-License-Identifier: GPL-2.0-or-later"""
import math
import sys

notes = ','.join(str(round(440 * 2 ** ((key - 69) / 12) * 2 ** 32 / 22050)) for key in range(128))
sine = ','.join(str(round(math.sin(i * math.pi / 128) * 127)) for i in range(256))
with open(sys.argv[1], 'w', newline='\n') as f:
    f.write('static const unsigned b310e_note_step[128] = {' + notes + '};\n'
            'static const int8_t b310e_sine[256] = {' + sine + '};\n')
