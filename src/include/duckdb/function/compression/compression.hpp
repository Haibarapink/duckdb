//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/compression/compression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function_set.hpp"
#include "duckdb/function/compression_function.hpp"

namespace duckdb {

struct RLEFun {
	static CompressionFunction GetFunction(PhysicalType type);
	static bool TypeIsSupported(PhysicalType type);
};

} // namespace duckdb
