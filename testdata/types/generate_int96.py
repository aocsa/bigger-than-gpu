#!/usr/bin/env python3
"""Write int96_timestamps.parquet: one required INT96 column, the legacy Impala/Spark timestamp.

DuckDB cannot write INT96, so this uses the hand-written writer in testdata/generate_parquet.py.
Run from this directory: python3 generate_int96.py
"""

import io
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from generate_parquet import (ENCODING_PLAIN, REPETITION_REQUIRED, write_file_metadata,
                              write_page_header)

PARQUET_INT96 = 3

# (Julian day, nanoseconds in the day): 1970-01-01 00:00:00, 2024-01-02 03:04:05.5,
# 1969-12-31 23:59:59.999999.
VALUES = [(2440588, 0), (2460312, 11_045_500_000_000), (2440587, 86_399_999_999_000)]


def main():
    out = io.BytesIO()
    out.write(b'PAR1')
    values = b''.join(struct.pack('<qi', nanos, day) for day, nanos in VALUES)
    page = io.BytesIO()
    write_page_header(page, 0, len(values), len(values), len(VALUES), ENCODING_PLAIN)
    page.write(values)
    offset = out.tell()
    out.write(page.getvalue())
    size = len(page.getvalue())

    schema = [
        {'name': 'schema', 'num_children': 1},
        {'name': 'ts', 'type': PARQUET_INT96, 'repetition': REPETITION_REQUIRED},
    ]
    row_groups = [{
        'columns': [{'file_offset': offset, 'type': PARQUET_INT96, 'encodings': [ENCODING_PLAIN],
                     'path': ['ts'], 'num_values': len(VALUES), 'total_size': size}],
        'total_byte_size': size,
        'num_rows': len(VALUES),
    }]
    metadata = io.BytesIO()
    write_file_metadata(metadata, schema, row_groups, len(VALUES))
    out.write(metadata.getvalue())
    out.write(struct.pack('<I', len(metadata.getvalue())))
    out.write(b'PAR1')
    with open('int96_timestamps.parquet', 'wb') as f:
        f.write(out.getvalue())


if __name__ == '__main__':
    main()
