#!/usr/bin/env python3
"""Generate test Parquet files for the C++ reader tests."""

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


def create_test_parquet():
    """Create a simple test Parquet file with int32 and double columns."""
    
    # Data values
    ids = [4, 5, 6, 7, 2, 3, 0, 1]
    scores = [1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5]
    num_values = len(ids)
    
    output = io.BytesIO()
    
    # Write magic
    output.write(b'PAR1')
    
    data_start = output.tell()
    
    # Write id column data page
    id_page_data = io.BytesIO()
    
    # Write page header
    id_values_data = struct.pack(f'<{num_values}i', *ids)
    write_page_header(id_page_data, 0, len(id_values_data), len(id_values_data), num_values, ENCODING_PLAIN)
    id_page_header_size = id_page_data.tell()
    
    # Write values
    id_page_data.write(id_values_data)
    
    id_column_offset = output.tell()
    id_column_data = id_page_data.getvalue()
    output.write(id_column_data)
    id_column_size = len(id_column_data)
    
    # Write score column data page
    score_page_data = io.BytesIO()
    
    score_values_data = struct.pack(f'<{num_values}d', *scores)
    write_page_header(score_page_data, 0, len(score_values_data), len(score_values_data), num_values, ENCODING_PLAIN)
    
    score_page_data.write(score_values_data)
    
    score_column_offset = output.tell()
    score_column_data = score_page_data.getvalue()
    output.write(score_column_data)
    score_column_size = len(score_column_data)
    
    # Build metadata
    schema_elements = [
        {'name': 'schema', 'num_children': 2},
        {'name': 'id', 'type': PARQUET_INT32, 'repetition': REPETITION_REQUIRED},
        {'name': 'score', 'type': PARQUET_DOUBLE, 'repetition': REPETITION_REQUIRED},
    ]
    
    row_groups = [{
        'columns': [
            {
                'file_offset': id_column_offset,
                'type': PARQUET_INT32,
                'encodings': [ENCODING_PLAIN],
                'path': ['id'],
                'num_values': num_values,
                'total_size': id_column_size,
            },
            {
                'file_offset': score_column_offset,
                'type': PARQUET_DOUBLE,
                'encodings': [ENCODING_PLAIN],
                'path': ['score'],
                'num_values': num_values,
                'total_size': score_column_size,
            },
        ],
        'total_byte_size': id_column_size + score_column_size,
        'num_rows': num_values,
    }]
    
    # Write file metadata
    metadata_start = output.tell()
    metadata_stream = io.BytesIO()
    write_file_metadata(metadata_stream, schema_elements, row_groups, num_values)
    metadata_bytes = metadata_stream.getvalue()
    output.write(metadata_bytes)
    
    # Write footer
    output.write(struct.pack('<I', len(metadata_bytes)))
    output.write(b'PAR1')
    
    return output.getvalue()


def main():
    parquet_data = create_test_parquet()
    
    with open('simple_test.parquet', 'wb') as f:
        f.write(parquet_data)
    
    print(f"Created simple_test.parquet ({len(parquet_data)} bytes)")
    print("Schema: id (INT32), score (DOUBLE)")
    print("Values: ids=[4,5,6,7,2,3,0,1], scores=[1.5,2.5,3.5,4.5,5.5,6.5,7.5,8.5]")


if __name__ == '__main__':
    main()
