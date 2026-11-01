#!/usr/bin/env python3
"""Write INT96 test files: one required INT96 column, the legacy Impala/Spark timestamp.

int96_timestamps.parquet holds PLAIN values; int96_dictionary.parquet holds a dictionary page
and a PLAIN_DICTIONARY data page, as Spark and Impala usually write INT96.

DuckDB cannot write INT96, so this uses the hand-written writer in testdata/generate_parquet.py.
Run from this directory: python3 generate_int96.py
"""

import io
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from generate_parquet import (ENCODING_PLAIN, REPETITION_REQUIRED, THRIFT_I32, THRIFT_STRUCT,
                              write_file_metadata, write_page_header, write_thrift_field,
                              write_thrift_stop, write_thrift_varint)

PARQUET_INT96 = 3
PAGE_DICTIONARY = 2
ENCODING_PLAIN_DICTIONARY = 2

# (Julian day, nanoseconds in the day): 1970-01-01 00:00:00, 2024-01-02 03:04:05.5,
# 1969-12-31 23:59:59.999999.
VALUES = [(2440588, 0), (2460312, 11_045_500_000_000), (2440587, 86_399_999_999_000)]


def int96(day, nanos):
    return struct.pack('<qi', nanos, day)


def dictionary_page(values):
    """A DICTIONARY_PAGE: PageHeader fields 1-3, then DictionaryPageHeader as field 7."""
    page = io.BytesIO()
    write_thrift_field(page, 1, THRIFT_I32, 0)
    write_thrift_varint(page, PAGE_DICTIONARY)
    write_thrift_field(page, 2, THRIFT_I32, 1)
    write_thrift_varint(page, len(values))
    write_thrift_field(page, 3, THRIFT_I32, 2)
    write_thrift_varint(page, len(values))
    write_thrift_field(page, 7, THRIFT_STRUCT, 3)
    write_thrift_field(page, 1, THRIFT_I32, 0)
    write_thrift_varint(page, len(values) // 12)
    write_thrift_field(page, 2, THRIFT_I32, 1)
    write_thrift_varint(page, ENCODING_PLAIN)
    write_thrift_stop(page)
    write_thrift_stop(page)
    page.write(values)
    return page.getvalue()


def write_file(path, column_bytes, num_values, encodings):
    out = io.BytesIO()
    out.write(b'PAR1')
    offset = out.tell()
    out.write(column_bytes)
    schema = [
        {'name': 'schema', 'num_children': 1},
        {'name': 'ts', 'type': PARQUET_INT96, 'repetition': REPETITION_REQUIRED},
    ]
    row_groups = [{
        'columns': [{'file_offset': offset, 'type': PARQUET_INT96, 'encodings': encodings,
                     'path': ['ts'], 'num_values': num_values,
                     'total_size': len(column_bytes)}],
        'total_byte_size': len(column_bytes),
        'num_rows': num_values,
    }]
    metadata = io.BytesIO()
    write_file_metadata(metadata, schema, row_groups, num_values)
    out.write(metadata.getvalue())
    out.write(struct.pack('<I', len(metadata.getvalue())))
    out.write(b'PAR1')
    with open(path, 'wb') as f:
        f.write(out.getvalue())


def main():
    values = b''.join(int96(day, nanos) for day, nanos in VALUES)
    page = io.BytesIO()
    write_page_header(page, 0, len(values), len(values), len(VALUES), ENCODING_PLAIN)
    page.write(values)
    write_file('int96_timestamps.parquet', page.getvalue(), len(VALUES), [ENCODING_PLAIN])

    # Dictionary [VALUES[1], VALUES[2]]; rows are indices 0, 1, 0, 0 as one bit-packed run:
    # bit width 1, run header (1 group << 1) | 1, then 8 one-bit indices LSB first.
    dictionary = int96(*VALUES[1]) + int96(*VALUES[2])
    indices = bytes([1, 0x03, 0b00000010])
    data = io.BytesIO()
    write_page_header(data, 0, len(indices), len(indices), 4, ENCODING_PLAIN_DICTIONARY)
    data.write(indices)
    write_file('int96_dictionary.parquet', dictionary_page(dictionary) + data.getvalue(), 4,
               [ENCODING_PLAIN_DICTIONARY, ENCODING_PLAIN])


if __name__ == '__main__':
    main()
