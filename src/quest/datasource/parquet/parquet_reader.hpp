// Copyright 2024
// Licensed under the Apache License, Version 2.0

#ifndef PARQUET_READER_HPP
#define PARQUET_READER_HPP

// Main include file for the Parquet reader library
// Include this file to get access to all public APIs

// Core datasource types (in quest::datasource namespace)
#include "quest/datasource/column_reader.hpp"
#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/data_source.hpp"
#include "quest/datasource/file_reader.hpp"
#include "quest/datasource/page_reader.hpp"
#include "quest/datasource/record_batch.hpp"
#include "quest/datasource/schema.hpp"
#include "quest/datasource/types.hpp"

// Parquet-specific types (in quest::datasource::parquet namespace)
#include "quest/datasource/parquet/compression.hpp"
#include "quest/datasource/parquet/encoding.hpp"
#include "quest/datasource/parquet/metadata.hpp"
#include "quest/datasource/parquet/thrift_decoder.hpp"

#endif  // PARQUET_READER_HPP
