#!/usr/bin/env python3
#
# Copyright © 2022 Etnaviv Project
# Copyright © 2024 Igalia, S.L.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

import textwrap
from isa import ISA, BitSetDerivedField, get_bitrange
import sys

xml = sys.argv[1]
dst = sys.argv[2]

isa = ISA(xml)


class Opcode(object):
    def __init__(self, bits, byte_hints=True, hint_lines=None):
        self.hint_lines = hint_lines or []
        self.byte_hints = byte_hints
        self.bits = [None] * bits
        self.colspan = [1] * bits
        self.borders = ['left right'] * bits
        self.left ='left'
        self.right = 'right'
        self.extra_row = [''] * bits
        self.extra_colspan = [1] * bits

    def add_constant(self, offset, size, value, name=None):
        assert (value & ~((1 << size) - 1)) == 0
        for i in range(size-2):
            self.borders[offset + 1 + i] = ''
        if size >= 2:
            self.borders[offset] = self.right
            self.borders[offset + size - 1] = self.left
        for i in range(size):
            assert self.bits[offset + i] is None
            self.bits[offset + i] = (value >> i) & 1

    def add_field(self, offset, size, name):
        on_extra_row = False
        for i in range(size):
            if self.bits[offset + i] is not None:
                on_extra_row = True
                break
        if on_extra_row:
            for i in range(size):
                assert not self.extra_row[offset + i]
                self.extra_row[offset + i] = name
                self.extra_colspan[offset + i] = size
        else:
            for i in range(size):
                assert self.bits[offset + i] is None
                self.bits[offset + i] = name
                self.colspan[offset + i] = size

    def to_html(self):
        for i in range(len(self.bits)-1):
            if self.bits[i] is None and self.bits[i+1] is None:
                self.borders[i] = self.borders[i].replace(self.left, '').strip()
                self.borders[i+1] = self.borders[i+1].replace(self.right, '').strip()

        parts = []

        parts.append('<div class="opcodewrapper notwrapped">')
        parts.append(self.to_html_line())
        parts.append('</div>')
        return ''.join(parts)

    def to_html_line(self, line_low=None, line_high=None):
        if line_low is None:
            line_low = 0
        if line_high is None:
            line_high = len(self.bits)

        start = line_high - 1
        end = line_low - 1
        step = -1

        parts = ['<table class="opcodebits">']

        parts.append('<thead><tr>')

        for i in  range(start, end, step):
            if (i+1) % 8 == 0 and self.byte_hints:
                parts.append('<td class="%s">%d</td>' % (self.left, i))
            elif i % 8 == 0 and self.byte_hints:
                parts.append('<td class="%s">%d</td>' % (self.right, i))
            elif i in self.hint_lines:
                parts.append('<td class="%s">%d</td>' % (self.left, i))
            else:
                parts.append('<td>%d</td>' % i)
        parts.append('</tr></thead>')

        parts.append('<tbody><tr>')

        o = start
        while o > end:
            i = self.bits[o]
            css_class = self.borders[o]
            if i is None:
                css_class = ('unknown ' + css_class).strip()
            parts.append('<td colspan="%d" class="%s">' % (self.colspan[o], css_class))
            if i is None:
                parts.append('x')
            elif isinstance(i, int):
                parts.append('%d' % i)
            elif isinstance(i, str):
                parts.append('%s' % i)
            parts.append('</td>')
            o -= self.colspan[o]

        parts.append('</tr>')
        parts.append('</tbody>')

        if any(self.extra_row):
            parts.append('<tfoot>')
            parts.append('<tr>')
            o = start
            while o > end:
                i = self.extra_row[o]
                css_class = 'left right' if i else ''
                parts.append('<td colspan="%d" class="%s">' % (self.extra_colspan[o], css_class))
                parts.append('%s' % i)
                parts.append('</td>')
                o -= self.extra_colspan[o]
            parts.append('</tr>')
            parts.append('</tbody>')


        parts.append('</table>')
        return ''.join(parts)

def trim_string(string):
    return textwrap.dedent(string).strip()

def create_rst_section(title, content):
    print(f"{title}\n{'-' * len(title)}\n\n{content}\n\n")

def create_rst_subsection(title):
    print(f"{title}\n{'~' * len(title)}\n\n")

def create_rst_table(data):
    col_widths = [max(len(cell) for cell in column) for column in zip(*data)]
    table_content = "\n".join("  ".join(cell.ljust(width) for cell, width in zip(row, col_widths)) for row in data)
    header_separator = "  ".join("=" * width for width in col_widths)

    print(f"{header_separator}\n{table_content}\n{header_separator}\n")

def print_introduction(section, description):
    create_rst_section(section, textwrap.dedent(description).strip())

def print_enum(name, enum):
    create_rst_subsection(name)

    if enum.doc is not None:
        print(f"{trim_string(enum.doc.text)}")
        print("")

    data = [
        ["Index", "Value"]
    ]

    for value_name, value in enum.values.items():
        data.append([value_name, value.get_name()])

    create_rst_table(data)

class Pattern(object):
    """Class that encapsulates a single bitset rule
    """
    def __init__(self, low, high, text):
        self.low = low
        self.high = high
        self.name = text

def instruction_bits(ins):
    ranges = []

    for pattern in ins.pattern:
        low, high = get_bitrange(pattern)
        ranges.append(Pattern(low, high, pattern.text.strip()))

    for case in ins.cases:
        for field_name, field in case.fields.items():
            if any(x.low == field.low for x in ranges) is False:
                ranges.append(field)

    if ins.extends is not None:
        parent = isa.bitsets[ins.extends]
        ranges += instruction_bits(parent)

    return ranges

def print_instruction(instr):
    create_rst_subsection(instr.name)

    ranges = instruction_bits(instr)
    builder = Opcode(64)

    for r in ranges:
        if isinstance(r, BitSetDerivedField):
            continue
        if hasattr(r, "type"):
            builder.add_field(r.low, r.high - r.low + 1, r.name)
        else:
            for idx in range(0, r.high - r.low + 1):
                if r.name[idx] == '0':
                    builder.add_constant(r.low + idx, 1, 0)
                elif r.name[idx] == '1':
                    builder.add_constant(r.low + idx, 1, 1)

    indented_html = "\n".join("   " + line for line in builder.to_html().splitlines())

    print('.. raw:: html')
    print('')
    print(indented_html)
    print('')

    if instr.doc is not None:
        print(f"{trim_string(instr.doc.text)}")
        print("")

    print('')

with open(dst, 'w') as f:
    sys.stdout = f

    print_introduction("Enumerations", """
            This section describes each enumeration used in the ISA.
            Enumerations are found in the instruction metadata and as
            modifiers in individual instructions.""")

    for enum_name, enum in isa.enums.items():
        print_enum(enum_name, enum)

    print_introduction("Instruction reference", """
            The following section each known instruction in the ISA.
            It contains the instruction name, syntax, and bit pattern.""")

    for instr in isa.instructions():
        print_instruction(instr)
