#!/usr/bin/env python3
"""Generate test Parquet file with string column and two row groups."""

import struct
import io

def write_thrift_varint(stream, value):
    """Write a zigzag-encoded varint."""
    # Zigzag encode
    encoded = (value << 1) ^ (value >> 31)
    while encoded > 0x7f:
        stream.write(bytes([0x80 | (encoded & 0x7f)]))
        encoded >>= 7
    stream.write(bytes([encoded & 0x7f]))

def write_thrift_uvarint(stream, value):
    """Write an unsigned varint."""
    while value > 0x7f:
        stream.write(bytes([0x80 | (value & 0x7f)]))
        value >>= 7
    stream.write(bytes([value & 0x7f]))

def write_thrift_string(stream, s):
    """Write a Thrift binary/string (length-prefixed)."""
    data = s.encode('utf-8') if isinstance(s, str) else s
    write_thrift_uvarint(stream, len(data))
    stream.write(data)

def write_thrift_field(stream, field_id, field_type, last_field_id=0):
    """Write a Thrift compact field header."""
    delta = field_id - last_field_id
    if 0 < delta <= 15:
        stream.write(bytes([(delta << 4) | field_type]))
    else:
        stream.write(bytes([field_type]))
        write_thrift_varint(stream, field_id)

def write_thrift_stop(stream):
    """Write Thrift struct stop marker."""
    stream.write(bytes([0]))

def write_thrift_list_header(stream, elem_type, size):
    """Write a Thrift list header."""
    if size < 15:
        stream.write(bytes([(size << 4) | elem_type]))
    else:
        stream.write(bytes([0xf0 | elem_type]))
        write_thrift_uvarint(stream, size)

# Thrift types for compact protocol
THRIFT_BOOL_TRUE = 1
THRIFT_BOOL_FALSE = 2
THRIFT_BYTE = 3
THRIFT_I16 = 4
THRIFT_I32 = 5
THRIFT_I64 = 6
THRIFT_DOUBLE = 7
THRIFT_BINARY = 8
THRIFT_LIST = 9
THRIFT_STRUCT = 12

# Parquet types
PARQUET_INT32 = 1
PARQUET_INT64 = 2
PARQUET_BOOLEAN = 0
PARQUET_DOUBLE = 5
PARQUET_BYTE_ARRAY = 6

# Repetition types
REPETITION_REQUIRED = 0
REPETITION_OPTIONAL = 1

# Encoding types
ENCODING_PLAIN = 0

def write_file_metadata(stream, schema_elements, row_groups, num_rows):
    """Write Parquet file metadata as Thrift."""
    last_field = 0

    # version = 1
    write_thrift_field(stream, 1, THRIFT_I32, last_field)
    write_thrift_varint(stream, 1)
    last_field = 1

    # schema
    write_thrift_field(stream, 2, THRIFT_LIST, last_field)
    write_thrift_list_header(stream, THRIFT_STRUCT, len(schema_elements))
    for elem in schema_elements:
        write_schema_element(stream, elem)
    last_field = 2

    # num_rows
    write_thrift_field(stream, 3, THRIFT_I64, last_field)
    write_thrift_varint(stream, num_rows)
    last_field = 3

    # row_groups
    write_thrift_field(stream, 4, THRIFT_LIST, last_field)
    write_thrift_list_header(stream, THRIFT_STRUCT, len(row_groups))
    for rg in row_groups:
        write_row_group(stream, rg)
    last_field = 4

    write_thrift_stop(stream)

def write_schema_element(stream, elem):
    """Write a schema element struct."""
    last_field = 0

    if 'type' in elem:
        write_thrift_field(stream, 1, THRIFT_I32, last_field)
        write_thrift_varint(stream, elem['type'])
        last_field = 1

    if 'repetition' in elem:
        write_thrift_field(stream, 3, THRIFT_I32, last_field)
        write_thrift_varint(stream, elem['repetition'])
        last_field = 3

    # name (required)
    write_thrift_field(stream, 4, THRIFT_BINARY, last_field)
    write_thrift_string(stream, elem['name'])
    last_field = 4

    if 'num_children' in elem:
        write_thrift_field(stream, 5, THRIFT_I32, last_field)
        write_thrift_varint(stream, elem['num_children'])
        last_field = 5

    write_thrift_stop(stream)

def write_row_group(stream, rg):
    """Write a row group struct."""
    last_field = 0

    # columns
    write_thrift_field(stream, 1, THRIFT_LIST, last_field)
    write_thrift_list_header(stream, THRIFT_STRUCT, len(rg['columns']))
    for col in rg['columns']:
        write_column_chunk(stream, col)
    last_field = 1

    # total_byte_size
    write_thrift_field(stream, 2, THRIFT_I64, last_field)
    write_thrift_varint(stream, rg['total_byte_size'])
    last_field = 2

    # num_rows
    write_thrift_field(stream, 3, THRIFT_I64, last_field)
    write_thrift_varint(stream, rg['num_rows'])
    last_field = 3

    write_thrift_stop(stream)

def write_column_chunk(stream, col):
    """Write a column chunk struct."""
    last_field = 0

    # file_offset
    write_thrift_field(stream, 2, THRIFT_I64, last_field)
    write_thrift_varint(stream, col['file_offset'])
    last_field = 2

    # meta_data
    write_thrift_field(stream, 3, THRIFT_STRUCT, last_field)
    write_column_metadata(stream, col)
    last_field = 3

    write_thrift_stop(stream)

def write_column_metadata(stream, col):
    """Write column chunk metadata struct."""
    last_field = 0

    # type
    write_thrift_field(stream, 1, THRIFT_I32, last_field)
    write_thrift_varint(stream, col['type'])
    last_field = 1

    # encodings
    write_thrift_field(stream, 2, THRIFT_LIST, last_field)
    write_thrift_list_header(stream, THRIFT_I32, len(col['encodings']))
    for enc in col['encodings']:
        write_thrift_varint(stream, enc)
    last_field = 2

    # path_in_schema
    write_thrift_field(stream, 3, THRIFT_LIST, last_field)
    write_thrift_list_header(stream, THRIFT_BINARY, len(col['path']))
    for p in col['path']:
        write_thrift_string(stream, p)
    last_field = 3

    # num_values
    write_thrift_field(stream, 5, THRIFT_I64, last_field)
    write_thrift_varint(stream, col['num_values'])
    last_field = 5

    # total_uncompressed_size
    write_thrift_field(stream, 6, THRIFT_I64, last_field)
    write_thrift_varint(stream, col['total_size'])
    last_field = 6

    # total_compressed_size
    write_thrift_field(stream, 7, THRIFT_I64, last_field)
    write_thrift_varint(stream, col['total_size'])
    last_field = 7

    # data_page_offset
    write_thrift_field(stream, 9, THRIFT_I64, last_field)
    write_thrift_varint(stream, col['file_offset'])
    last_field = 9

    write_thrift_stop(stream)

def write_page_header(stream, page_type, uncompressed_size, compressed_size, num_values, encoding):
    """Write a data page header."""
    last_field = 0

    # type
    write_thrift_field(stream, 1, THRIFT_I32, last_field)
    write_thrift_varint(stream, page_type)
    last_field = 1

    # uncompressed_page_size
    write_thrift_field(stream, 2, THRIFT_I32, last_field)
    write_thrift_varint(stream, uncompressed_size)
    last_field = 2

    # compressed_page_size
    write_thrift_field(stream, 3, THRIFT_I32, last_field)
    write_thrift_varint(stream, compressed_size)
    last_field = 3

    # data_page_header (field 5)
    write_thrift_field(stream, 5, THRIFT_STRUCT, last_field)
    write_data_page_header(stream, num_values, encoding)
    last_field = 5

    write_thrift_stop(stream)

def write_data_page_header(stream, num_values, encoding):
    """Write data page header struct."""
    last_field = 0

    # num_values
    write_thrift_field(stream, 1, THRIFT_I32, last_field)
    write_thrift_varint(stream, num_values)
    last_field = 1

    # encoding
    write_thrift_field(stream, 2, THRIFT_I32, last_field)
    write_thrift_varint(stream, encoding)
    last_field = 2

    # definition_level_encoding (RLE = 3)
    write_thrift_field(stream, 3, THRIFT_I32, last_field)
    write_thrift_varint(stream, 3)
    last_field = 3

    # repetition_level_encoding (RLE = 3)
    write_thrift_field(stream, 4, THRIFT_I32, last_field)
    write_thrift_varint(stream, 3)
    last_field = 4

    write_thrift_stop(stream)


def encode_byte_array_values(strings):
    """Encode strings as Parquet BYTE_ARRAY (length-prefixed)."""
    result = io.BytesIO()
    for s in strings:
        data = s.encode('utf-8')
        result.write(struct.pack('<I', len(data)))
        result.write(data)
    return result.getvalue()


def write_column_page(output, values, parquet_type, num_values):
    """Write a column's data page and return (offset, size)."""
    page_data = io.BytesIO()

    if parquet_type == PARQUET_INT32:
        values_data = struct.pack(f'<{num_values}i', *values)
    elif parquet_type == PARQUET_DOUBLE:
        values_data = struct.pack(f'<{num_values}d', *values)
    elif parquet_type == PARQUET_BYTE_ARRAY:
        values_data = encode_byte_array_values(values)
    else:
        raise ValueError(f"Unsupported type: {parquet_type}")

    write_page_header(page_data, 0, len(values_data), len(values_data), num_values, ENCODING_PLAIN)
    page_data.write(values_data)

    offset = output.tell()
    data = page_data.getvalue()
    output.write(data)
    return offset, len(data)


def create_test_parquet():
    """Create a test Parquet file with id, score, name columns and two row groups."""

    # Row group 1 data
    ids1 = [1, 2, 3, 4]
    scores1 = [10.5, 20.5, 30.5, 40.5]
    names1 = ["Alice", "Bob", "Charlie", "Diana"]
    num_values1 = len(ids1)

    # Row group 2 data
    ids2 = [5, 6, 7]
    scores2 = [50.5, 60.5, 70.5]
    names2 = ["Eve", "Frank", "Grace"]
    num_values2 = len(ids2)

    total_rows = num_values1 + num_values2

    output = io.BytesIO()

    # Write magic
    output.write(b'PAR1')

    # Write row group 1 columns
    id1_offset, id1_size = write_column_page(output, ids1, PARQUET_INT32, num_values1)
    score1_offset, score1_size = write_column_page(output, scores1, PARQUET_DOUBLE, num_values1)
    name1_offset, name1_size = write_column_page(output, names1, PARQUET_BYTE_ARRAY, num_values1)

    # Write row group 2 columns
    id2_offset, id2_size = write_column_page(output, ids2, PARQUET_INT32, num_values2)
    score2_offset, score2_size = write_column_page(output, scores2, PARQUET_DOUBLE, num_values2)
    name2_offset, name2_size = write_column_page(output, names2, PARQUET_BYTE_ARRAY, num_values2)

    # Build metadata
    schema_elements = [
        {'name': 'schema', 'num_children': 3},
        {'name': 'id', 'type': PARQUET_INT32, 'repetition': REPETITION_REQUIRED},
        {'name': 'score', 'type': PARQUET_DOUBLE, 'repetition': REPETITION_REQUIRED},
        {'name': 'name', 'type': PARQUET_BYTE_ARRAY, 'repetition': REPETITION_REQUIRED},
    ]

    row_groups = [
        {
            'columns': [
                {
                    'file_offset': id1_offset,
                    'type': PARQUET_INT32,
                    'encodings': [ENCODING_PLAIN],
                    'path': ['id'],
                    'num_values': num_values1,
                    'total_size': id1_size,
                },
                {
                    'file_offset': score1_offset,
                    'type': PARQUET_DOUBLE,
                    'encodings': [ENCODING_PLAIN],
                    'path': ['score'],
                    'num_values': num_values1,
                    'total_size': score1_size,
                },
                {
                    'file_offset': name1_offset,
                    'type': PARQUET_BYTE_ARRAY,
                    'encodings': [ENCODING_PLAIN],
                    'path': ['name'],
                    'num_values': num_values1,
                    'total_size': name1_size,
                },
            ],
            'total_byte_size': id1_size + score1_size + name1_size,
            'num_rows': num_values1,
        },
        {
            'columns': [
                {
                    'file_offset': id2_offset,
                    'type': PARQUET_INT32,
                    'encodings': [ENCODING_PLAIN],
                    'path': ['id'],
                    'num_values': num_values2,
                    'total_size': id2_size,
                },
                {
                    'file_offset': score2_offset,
                    'type': PARQUET_DOUBLE,
                    'encodings': [ENCODING_PLAIN],
                    'path': ['score'],
                    'num_values': num_values2,
                    'total_size': score2_size,
                },
                {
                    'file_offset': name2_offset,
                    'type': PARQUET_BYTE_ARRAY,
                    'encodings': [ENCODING_PLAIN],
                    'path': ['name'],
                    'num_values': num_values2,
                    'total_size': name2_size,
                },
            ],
            'total_byte_size': id2_size + score2_size + name2_size,
            'num_rows': num_values2,
        },
    ]

    # Write file metadata
    metadata_stream = io.BytesIO()
    write_file_metadata(metadata_stream, schema_elements, row_groups, total_rows)
    metadata_bytes = metadata_stream.getvalue()
    output.write(metadata_bytes)

    # Write footer
    output.write(struct.pack('<I', len(metadata_bytes)))
    output.write(b'PAR1')

    return output.getvalue()


def main():
    parquet_data = create_test_parquet()

    with open('string_test.parquet', 'wb') as f:
        f.write(parquet_data)

    print(f"Created string_test.parquet ({len(parquet_data)} bytes)")
    print("Schema: id (INT32), score (DOUBLE), name (BYTE_ARRAY/STRING)")
    print("Row group 1 (4 rows): ids=[1,2,3,4], scores=[10.5,20.5,30.5,40.5], names=[Alice,Bob,Charlie,Diana]")
    print("Row group 2 (3 rows): ids=[5,6,7], scores=[50.5,60.5,70.5], names=[Eve,Frank,Grace]")


if __name__ == '__main__':
    main()
