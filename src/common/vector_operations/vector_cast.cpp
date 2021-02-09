#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/chunk_collection.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

template <class SRC, class OP>
static void VectorStringCast(Vector &source, Vector &result, idx_t count) {
	D_ASSERT(result.buffer->type.InternalType() == PhysicalType::VARCHAR);
	UnaryExecutor::Execute<SRC, string_t, true>(source, result, count,
	                                            [&](SRC input) { return OP::template Operation<SRC>(input, result); });
}

static NotImplementedException UnimplementedCast(const LogicalType &source_type, const LogicalType &target_type) {
	return NotImplementedException("Unimplemented type for cast (%s -> %s)", source_type.ToString(),
	                               target_type.ToString());
}

// NULL cast only works if all values in source are NULL, otherwise an unimplemented cast exception is thrown
static void VectorNullCast(Vector &source, Vector &result, idx_t count) {
	if (VectorOperations::HasNotNull(source, count)) {
		throw UnimplementedCast(source.buffer->type, result.buffer->type);
	}
	if (source.buffer->vector_type == VectorType::CONSTANT_VECTOR) {
		result.buffer->vector_type = VectorType::CONSTANT_VECTOR;
		ConstantVector::SetNull(result, true);
	} else {
		result.buffer->vector_type = VectorType::FLAT_VECTOR;
		FlatVector::Nullmask(result).set();
	}
}

template <class T>
static void ToDecimalCast(Vector &source, Vector &result, idx_t count) {
	switch (result.buffer->type.InternalType()) {
	case PhysicalType::INT16:
		UnaryExecutor::Execute<T, int16_t, true>(source, result, count, [&](T input) {
			return CastToDecimal::Operation<T, int16_t>(input, result.buffer->type.width(),
			                                            result.buffer->type.scale());
		});
		break;
	case PhysicalType::INT32:
		UnaryExecutor::Execute<T, int32_t, true>(source, result, count, [&](T input) {
			return CastToDecimal::Operation<T, int32_t>(input, result.buffer->type.width(),
			                                            result.buffer->type.scale());
		});
		break;
	case PhysicalType::INT64:
		UnaryExecutor::Execute<T, int64_t, true>(source, result, count, [&](T input) {
			return CastToDecimal::Operation<T, int64_t>(input, result.buffer->type.width(),
			                                            result.buffer->type.scale());
		});
		break;
	case PhysicalType::INT128:
		UnaryExecutor::Execute<T, hugeint_t, true>(source, result, count, [&](T input) {
			return CastToDecimal::Operation<T, hugeint_t>(input, result.buffer->type.width(),
			                                              result.buffer->type.scale());
		});
		break;
	default:
		throw NotImplementedException("Unimplemented internal type for decimal");
	}
}

template <class T>
static void FromDecimalCast(Vector &source, Vector &result, idx_t count) {
	switch (source.buffer->type.InternalType()) {
	case PhysicalType::INT16:
		UnaryExecutor::Execute<int16_t, T, true>(source, result, count, [&](int16_t input) {
			return CastFromDecimal::Operation<int16_t, T>(input, source.buffer->type.width(),
			                                              source.buffer->type.scale());
		});
		break;
	case PhysicalType::INT32:
		UnaryExecutor::Execute<int32_t, T, true>(source, result, count, [&](int32_t input) {
			return CastFromDecimal::Operation<int32_t, T>(input, source.buffer->type.width(),
			                                              source.buffer->type.scale());
		});
		break;
	case PhysicalType::INT64:
		UnaryExecutor::Execute<int64_t, T, true>(source, result, count, [&](int64_t input) {
			return CastFromDecimal::Operation<int64_t, T>(input, source.buffer->type.width(),
			                                              source.buffer->type.scale());
		});
		break;
	case PhysicalType::INT128:
		UnaryExecutor::Execute<hugeint_t, T, true>(source, result, count, [&](hugeint_t input) {
			return CastFromDecimal::Operation<hugeint_t, T>(input, source.buffer->type.width(),
			                                                source.buffer->type.scale());
		});
		break;
	default:
		throw NotImplementedException("Unimplemented internal type for decimal");
	}
}

template <class SOURCE, class DEST, class POWERS_SOURCE, class POWERS_DEST>
void TemplatedDecimalScaleUp(Vector &source, Vector &result, idx_t count) {
	D_ASSERT(result.buffer->type.scale() >= source.buffer->type.scale());
	idx_t scale_difference = result.buffer->type.scale() - source.buffer->type.scale();
	auto multiply_factor = POWERS_DEST::POWERS_OF_TEN[scale_difference];
	idx_t target_width = result.buffer->type.width() - scale_difference;
	if (source.buffer->type.width() < target_width) {
		// type will always fit: no need to check limit
		UnaryExecutor::Execute<SOURCE, DEST, true>(source, result, count, [&](SOURCE input) {
			return Cast::Operation<SOURCE, DEST>(input) * multiply_factor;
		});
	} else {
		// type might not fit: check limit
		auto limit = POWERS_SOURCE::POWERS_OF_TEN[target_width];
		UnaryExecutor::Execute<SOURCE, DEST, true>(source, result, count, [&](SOURCE input) {
			if (input >= limit || input <= -limit) {
				throw OutOfRangeException("Casting value \"%s\" to type %s failed: value is out of range!",
				                          Decimal::ToString(input, source.buffer->type.scale()),
				                          result.buffer->type.ToString());
			}
			return Cast::Operation<SOURCE, DEST>(input) * multiply_factor;
		});
	}
}

template <class SOURCE, class DEST, class POWERS_SOURCE>
void TemplatedDecimalScaleDown(Vector &source, Vector &result, idx_t count) {
	D_ASSERT(result.buffer->type.scale() < source.buffer->type.scale());
	idx_t scale_difference = source.buffer->type.scale() - result.buffer->type.scale();
	idx_t target_width = result.buffer->type.width() + scale_difference;
	auto divide_factor = POWERS_SOURCE::POWERS_OF_TEN[scale_difference];
	if (source.buffer->type.width() < target_width) {
		// type will always fit: no need to check limit
		UnaryExecutor::Execute<SOURCE, DEST, true>(
		    source, result, count, [&](SOURCE input) { return Cast::Operation<SOURCE, DEST>(input / divide_factor); });
	} else {
		// type might not fit: check limit
		auto limit = POWERS_SOURCE::POWERS_OF_TEN[target_width];
		UnaryExecutor::Execute<SOURCE, DEST, true>(source, result, count, [&](SOURCE input) {
			if (input >= limit || input <= -limit) {
				throw OutOfRangeException("Casting value \"%s\" to type %s failed: value is out of range!",
				                          Decimal::ToString(input, source.buffer->type.scale()),
				                          result.buffer->type.ToString());
			}
			return Cast::Operation<SOURCE, DEST>(input / divide_factor);
		});
	}
}

template <class SOURCE, class POWERS_SOURCE>
static void DecimalDecimalCastSwitch(Vector &source, Vector &result, idx_t count) {
	source.buffer->type.Verify();
	result.buffer->type.Verify();

	// we need to either multiply or divide by the difference in scales
	if (result.buffer->type.scale() >= source.buffer->type.scale()) {
		// multiply
		switch (result.buffer->type.InternalType()) {
		case PhysicalType::INT16:
			TemplatedDecimalScaleUp<SOURCE, int16_t, POWERS_SOURCE, NumericHelper>(source, result, count);
			break;
		case PhysicalType::INT32:
			TemplatedDecimalScaleUp<SOURCE, int32_t, POWERS_SOURCE, NumericHelper>(source, result, count);
			break;
		case PhysicalType::INT64:
			TemplatedDecimalScaleUp<SOURCE, int64_t, POWERS_SOURCE, NumericHelper>(source, result, count);
			break;
		case PhysicalType::INT128:
			TemplatedDecimalScaleUp<SOURCE, hugeint_t, POWERS_SOURCE, Hugeint>(source, result, count);
			break;
		default:
			throw NotImplementedException("Unimplemented internal type for decimal");
		}
	} else {
		// divide
		switch (result.buffer->type.InternalType()) {
		case PhysicalType::INT16:
			TemplatedDecimalScaleDown<SOURCE, int16_t, POWERS_SOURCE>(source, result, count);
			break;
		case PhysicalType::INT32:
			TemplatedDecimalScaleDown<SOURCE, int32_t, POWERS_SOURCE>(source, result, count);
			break;
		case PhysicalType::INT64:
			TemplatedDecimalScaleDown<SOURCE, int64_t, POWERS_SOURCE>(source, result, count);
			break;
		case PhysicalType::INT128:
			TemplatedDecimalScaleDown<SOURCE, hugeint_t, POWERS_SOURCE>(source, result, count);
			break;
		default:
			throw NotImplementedException("Unimplemented internal type for decimal");
		}
	}
}

static void DecimalCastSwitch(Vector &source, Vector &result, idx_t count) {
	// now switch on the result type
	switch (result.buffer->type.id()) {
	case LogicalTypeId::BOOLEAN:
		FromDecimalCast<bool>(source, result, count);
		break;
	case LogicalTypeId::TINYINT:
		FromDecimalCast<int8_t>(source, result, count);
		break;
	case LogicalTypeId::SMALLINT:
		FromDecimalCast<int16_t>(source, result, count);
		break;
	case LogicalTypeId::INTEGER:
		FromDecimalCast<int32_t>(source, result, count);
		break;
	case LogicalTypeId::BIGINT:
		FromDecimalCast<int64_t>(source, result, count);
		break;
	case LogicalTypeId::UTINYINT:
		FromDecimalCast<uint8_t>(source, result, count);
		break;
	case LogicalTypeId::USMALLINT:
		FromDecimalCast<uint16_t>(source, result, count);
		break;
	case LogicalTypeId::UINTEGER:
		FromDecimalCast<uint32_t>(source, result, count);
		break;
	case LogicalTypeId::UBIGINT:
		FromDecimalCast<uint64_t>(source, result, count);
		break;
	case LogicalTypeId::HUGEINT:
		FromDecimalCast<hugeint_t>(source, result, count);
		break;
	case LogicalTypeId::DECIMAL: {
		// decimal to decimal cast
		// first we need to figure out the source and target internal types
		switch (source.buffer->type.InternalType()) {
		case PhysicalType::INT16:
			DecimalDecimalCastSwitch<int16_t, NumericHelper>(source, result, count);
			break;
		case PhysicalType::INT32:
			DecimalDecimalCastSwitch<int32_t, NumericHelper>(source, result, count);
			break;
		case PhysicalType::INT64:
			DecimalDecimalCastSwitch<int64_t, NumericHelper>(source, result, count);
			break;
		case PhysicalType::INT128:
			DecimalDecimalCastSwitch<hugeint_t, Hugeint>(source, result, count);
			break;
		default:
			throw NotImplementedException("Unimplemented internal type for decimal in decimal_decimal cast");
		}
		break;
	}
	case LogicalTypeId::FLOAT:
		FromDecimalCast<float>(source, result, count);
		break;
	case LogicalTypeId::DOUBLE:
		FromDecimalCast<double>(source, result, count);
		break;
	case LogicalTypeId::VARCHAR: {
		switch (source.buffer->type.InternalType()) {
		case PhysicalType::INT16:
			UnaryExecutor::Execute<int16_t, string_t, true>(source, result, count, [&](int16_t input) {
				return StringCastFromDecimal::Operation<int16_t>(input, source.buffer->type.width(),
				                                                 source.buffer->type.scale(), result);
			});
			break;
		case PhysicalType::INT32:
			UnaryExecutor::Execute<int32_t, string_t, true>(source, result, count, [&](int32_t input) {
				return StringCastFromDecimal::Operation<int32_t>(input, source.buffer->type.width(),
				                                                 source.buffer->type.scale(), result);
			});
			break;
		case PhysicalType::INT64:
			UnaryExecutor::Execute<int64_t, string_t, true>(source, result, count, [&](int64_t input) {
				return StringCastFromDecimal::Operation<int64_t>(input, source.buffer->type.width(),
				                                                 source.buffer->type.scale(), result);
			});
			break;
		case PhysicalType::INT128:
			UnaryExecutor::Execute<hugeint_t, string_t, true>(source, result, count, [&](hugeint_t input) {
				return StringCastFromDecimal::Operation<hugeint_t>(input, source.buffer->type.width(),
				                                                   source.buffer->type.scale(), result);
			});
			break;
		default:
			throw NotImplementedException("Unimplemented internal decimal type");
		}
		break;
	}
	default:
		VectorNullCast(source, result, count);
		break;
	}
}

template <class SRC>
static void NumericCastSwitch(Vector &source, Vector &result, idx_t count) {
	// now switch on the result type
	switch (result.buffer->type.id()) {
	case LogicalTypeId::BOOLEAN:
		UnaryExecutor::Execute<SRC, bool, duckdb::Cast, true>(source, result, count);
		break;
	case LogicalTypeId::TINYINT:
		UnaryExecutor::Execute<SRC, int8_t, duckdb::Cast, true>(source, result, count);
		break;
	case LogicalTypeId::SMALLINT:
		UnaryExecutor::Execute<SRC, int16_t, duckdb::Cast, true>(source, result, count);
		break;
	case LogicalTypeId::INTEGER:
		UnaryExecutor::Execute<SRC, int32_t, duckdb::Cast, true>(source, result, count);
		break;
	case LogicalTypeId::BIGINT:
		UnaryExecutor::Execute<SRC, int64_t, duckdb::Cast, true>(source, result, count);
		break;
	case LogicalTypeId::UTINYINT:
		UnaryExecutor::Execute<SRC, uint8_t, duckdb::Cast, true>(source, result, count);
		break;
	case LogicalTypeId::USMALLINT:
		UnaryExecutor::Execute<SRC, uint16_t, duckdb::Cast, true>(source, result, count);
		break;
	case LogicalTypeId::UINTEGER:
		UnaryExecutor::Execute<SRC, uint32_t, duckdb::Cast, true>(source, result, count);
		break;
	case LogicalTypeId::UBIGINT:
		UnaryExecutor::Execute<SRC, uint64_t, duckdb::Cast, true>(source, result, count);
		break;
	case LogicalTypeId::HUGEINT:
		UnaryExecutor::Execute<SRC, hugeint_t, duckdb::Cast, true>(source, result, count);
		break;
	case LogicalTypeId::FLOAT:
		UnaryExecutor::Execute<SRC, float, duckdb::Cast, true>(source, result, count);
		break;
	case LogicalTypeId::DOUBLE:
		UnaryExecutor::Execute<SRC, double, duckdb::Cast, true>(source, result, count);
		break;
	case LogicalTypeId::DECIMAL:
		ToDecimalCast<SRC>(source, result, count);
		break;
	case LogicalTypeId::VARCHAR: {
		VectorStringCast<SRC, duckdb::StringCast>(source, result, count);
		break;
	}
	case LogicalTypeId::LIST: {
		auto list_child = make_unique<ChunkCollection>();
		ListVector::SetEntry(result, move(list_child));
		VectorNullCast(source, result, count);
		break;
	}
	default:
		VectorNullCast(source, result, count);
		break;
	}
}

template <class OP>
static void VectorStringCastNumericSwitch(Vector &source, Vector &result, idx_t count) {
	// now switch on the result type
	switch (result.buffer->type.id()) {
	case LogicalTypeId::BOOLEAN:
		UnaryExecutor::Execute<string_t, bool, OP, true>(source, result, count);
		break;
	case LogicalTypeId::TINYINT:
		UnaryExecutor::Execute<string_t, int8_t, OP, true>(source, result, count);
		break;
	case LogicalTypeId::SMALLINT:
		UnaryExecutor::Execute<string_t, int16_t, OP, true>(source, result, count);
		break;
	case LogicalTypeId::INTEGER:
		UnaryExecutor::Execute<string_t, int32_t, OP, true>(source, result, count);
		break;
	case LogicalTypeId::BIGINT:
		UnaryExecutor::Execute<string_t, int64_t, OP, true>(source, result, count);
		break;
	case LogicalTypeId::UTINYINT:
		UnaryExecutor::Execute<string_t, uint8_t, OP, true>(source, result, count);
		break;
	case LogicalTypeId::USMALLINT:
		UnaryExecutor::Execute<string_t, uint16_t, OP, true>(source, result, count);
		break;
	case LogicalTypeId::UINTEGER:
		UnaryExecutor::Execute<string_t, uint32_t, OP, true>(source, result, count);
		break;
	case LogicalTypeId::UBIGINT:
		UnaryExecutor::Execute<string_t, uint64_t, OP, true>(source, result, count);
		break;
	case LogicalTypeId::HUGEINT:
		UnaryExecutor::Execute<string_t, hugeint_t, OP, true>(source, result, count);
		break;
	case LogicalTypeId::FLOAT:
		UnaryExecutor::Execute<string_t, float, OP, true>(source, result, count);
		break;
	case LogicalTypeId::DOUBLE:
		UnaryExecutor::Execute<string_t, double, OP, true>(source, result, count);
		break;
	case LogicalTypeId::INTERVAL:
		UnaryExecutor::Execute<string_t, interval_t, OP, true>(source, result, count);
		break;
	case LogicalTypeId::DECIMAL:
		ToDecimalCast<string_t>(source, result, count);
		break;
	default:
		VectorNullCast(source, result, count);
		break;
	}
}

static void StringCastSwitch(Vector &source, Vector &result, idx_t count, bool strict = false) {
	// now switch on the result type
	switch (result.buffer->type.id()) {
	case LogicalTypeId::DATE:
		if (strict) {
			UnaryExecutor::Execute<string_t, date_t, duckdb::StrictCastToDate, true>(source, result, count);
		} else {
			UnaryExecutor::Execute<string_t, date_t, duckdb::CastToDate, true>(source, result, count);
		}
		break;
	case LogicalTypeId::TIME:
		if (strict) {
			UnaryExecutor::Execute<string_t, dtime_t, duckdb::StrictCastToTime, true>(source, result, count);
		} else {
			UnaryExecutor::Execute<string_t, dtime_t, duckdb::CastToTime, true>(source, result, count);
		}
		break;
	case LogicalTypeId::TIMESTAMP:
		UnaryExecutor::Execute<string_t, timestamp_t, duckdb::CastToTimestamp, true>(source, result, count);
		break;
	case LogicalTypeId::BLOB:
		VectorStringCast<string_t, duckdb::CastToBlob>(source, result, count);
		break;
	default:
		if (strict) {
			VectorStringCastNumericSwitch<duckdb::StrictCast>(source, result, count);
		} else {
			VectorStringCastNumericSwitch<duckdb::Cast>(source, result, count);
		}
		break;
	}
}

static void DateCastSwitch(Vector &source, Vector &result, idx_t count) {
	// now switch on the result type
	switch (result.buffer->type.id()) {
	case LogicalTypeId::VARCHAR:
		// date to varchar
		VectorStringCast<date_t, duckdb::CastFromDate>(source, result, count);
		break;
	case LogicalTypeId::TIMESTAMP:
		// date to timestamp
		UnaryExecutor::Execute<date_t, timestamp_t, duckdb::CastDateToTimestamp, true>(source, result, count);
		break;
	default:
		VectorNullCast(source, result, count);
		break;
	}
}

static void TimeCastSwitch(Vector &source, Vector &result, idx_t count) {
	// now switch on the result type
	switch (result.buffer->type.id()) {
	case LogicalTypeId::VARCHAR:
		// time to varchar
		VectorStringCast<dtime_t, duckdb::CastFromTime>(source, result, count);
		break;
	default:
		VectorNullCast(source, result, count);
		break;
	}
}

static void TimestampCastSwitch(Vector &source, Vector &result, idx_t count) {
	// now switch on the result type
	switch (result.buffer->type.id()) {
	case LogicalTypeId::VARCHAR:
		// timestamp to varchar
		VectorStringCast<timestamp_t, duckdb::CastFromTimestamp>(source, result, count);
		break;
	case LogicalTypeId::DATE:
		// timestamp to date
		UnaryExecutor::Execute<timestamp_t, date_t, duckdb::CastTimestampToDate, true>(source, result, count);
		break;
	case LogicalTypeId::TIME:
		// timestamp to time
		UnaryExecutor::Execute<timestamp_t, dtime_t, duckdb::CastTimestampToTime, true>(source, result, count);
		break;
	default:
		VectorNullCast(source, result, count);
		break;
	}
}

static void IntervalCastSwitch(Vector &source, Vector &result, idx_t count) {
	// now switch on the result type
	switch (result.buffer->type.id()) {
	case LogicalTypeId::VARCHAR:
		// time to varchar
		VectorStringCast<interval_t, duckdb::StringCast>(source, result, count);
		break;
	default:
		VectorNullCast(source, result, count);
		break;
	}
}

static void BlobCastSwitch(Vector &source, Vector &result, idx_t count) {
	// now switch on the result type
	switch (result.buffer->type.id()) {
	case LogicalTypeId::VARCHAR:
		// blob to varchar
		VectorStringCast<string_t, duckdb::CastFromBlob>(source, result, count);
		break;
	default:
		VectorNullCast(source, result, count);
		break;
	}
}

static void ValueStringCastSwitch(Vector &source, Vector &result, idx_t count) {
	switch (result.buffer->type.id()) {
	case LogicalTypeId::VARCHAR:
		if (source.buffer->vector_type == VectorType::CONSTANT_VECTOR) {
			result.buffer->vector_type = source.buffer->vector_type;
		} else {
			result.buffer->vector_type = VectorType::FLAT_VECTOR;
		}
		for (idx_t i = 0; i < count; i++) {
			auto src_val = source.GetValue(i);
			auto str_val = src_val.ToString();
			result.SetValue(i, Value(str_val));
		}
		break;
	default:
		VectorNullCast(source, result, count);
		break;
	}
}

static void ListCastSwitch(Vector &source, Vector &result, idx_t count) {
	switch (result.buffer->type.id()) {
	case LogicalTypeId::LIST: {
		// only handle constant and flat vectors here for now
		if (source.buffer->vector_type == VectorType::CONSTANT_VECTOR) {
			result.buffer->vector_type = source.buffer->vector_type;
			ConstantVector::SetNull(result, ConstantVector::IsNull(source));
		} else {
			source.Normalify(count);
			result.buffer->vector_type = VectorType::FLAT_VECTOR;
			FlatVector::SetNullmask(result, FlatVector::Nullmask(source));
		}
		auto list_child = make_unique<ChunkCollection>();
		if (ListVector::HasEntry(source)) {
			auto &source_cc = ListVector::GetEntry(source);
			auto &target_cc = *list_child;
			// convert the entire chunk collection
			vector<LogicalType> result_types;
			result_types.push_back(result.buffer->type.child_types()[0].second);
			DataChunk append_chunk;
			append_chunk.Initialize(result_types);
			for (auto &chunk : source_cc.Chunks()) {
				VectorOperations::Cast(chunk->data[0], append_chunk.data[0], chunk->size());
				append_chunk.SetCardinality(chunk->size());
				target_cc.Append(append_chunk);
			}
		}
		ListVector::SetEntry(result, move(list_child));
		auto ldata = FlatVector::GetData<list_entry_t>(source);
		auto tdata = FlatVector::GetData<list_entry_t>(result);
		for (idx_t i = 0; i < count; i++) {
			tdata[i] = ldata[i];
		}
		break;
	}
	default:
		ValueStringCastSwitch(source, result, count);
		break;
	}
}

static void StructCastSwitch(Vector &source, Vector &result, idx_t count) {
	switch (result.buffer->type.id()) {
	case LogicalTypeId::STRUCT: {
		if (source.buffer->type.child_types().size() != result.buffer->type.child_types().size()) {
			throw TypeMismatchException(source.buffer->type, result.buffer->type,
			                            "Cannot cast STRUCTs of different size");
		}
		auto &source_children = StructVector::GetEntries(source);
		D_ASSERT(source_children.size() == source.buffer->type.child_types().size());

		bool is_constant = true;
		for (idx_t c_idx = 0; c_idx < result.buffer->type.child_types().size(); c_idx++) {
			auto &child_type = result.buffer->type.child_types()[c_idx];
			auto result_child_vector = make_unique<Vector>(child_type.second);
			auto &source_child_vector = *source_children[c_idx].second;
			if (source_child_vector.buffer->vector_type != VectorType::CONSTANT_VECTOR) {
				is_constant = false;
			}
			if (child_type.second != source_child_vector.buffer->type) {
				VectorOperations::Cast(source_child_vector, *result_child_vector, count, false);
			} else {
				result_child_vector->Reference(source_child_vector);
			}
			StructVector::AddEntry(result, child_type.first, move(result_child_vector));
		}
		if (is_constant) {
			result.buffer->vector_type = VectorType::CONSTANT_VECTOR;
		}

		break;
	}
	case LogicalTypeId::VARCHAR:
		if (source.buffer->vector_type == VectorType::CONSTANT_VECTOR) {
			result.buffer->vector_type = source.buffer->vector_type;
		} else {
			result.buffer->vector_type = VectorType::FLAT_VECTOR;
		}
		for (idx_t i = 0; i < count; i++) {
			auto src_val = source.GetValue(i);
			auto str_val = src_val.ToString();
			result.SetValue(i, Value(str_val));
		}
		break;

	default:
		VectorNullCast(source, result, count);
		break;
	}
}

void VectorOperations::Cast(Vector &source, Vector &result, idx_t count, bool strict) {
	D_ASSERT(source.buffer->type != result.buffer->type);
	// first switch on source type
	switch (source.buffer->type.id()) {
	case LogicalTypeId::BOOLEAN:
		NumericCastSwitch<bool>(source, result, count);
		break;
	case LogicalTypeId::TINYINT:
		NumericCastSwitch<int8_t>(source, result, count);
		break;
	case LogicalTypeId::SMALLINT:
		NumericCastSwitch<int16_t>(source, result, count);
		break;
	case LogicalTypeId::INTEGER:
		NumericCastSwitch<int32_t>(source, result, count);
		break;
	case LogicalTypeId::BIGINT:
		NumericCastSwitch<int64_t>(source, result, count);
		break;
	case LogicalTypeId::UTINYINT:
		NumericCastSwitch<uint8_t>(source, result, count);
		break;
	case LogicalTypeId::USMALLINT:
		NumericCastSwitch<uint16_t>(source, result, count);
		break;
	case LogicalTypeId::UINTEGER:
		NumericCastSwitch<uint32_t>(source, result, count);
		break;
	case LogicalTypeId::UBIGINT:
		NumericCastSwitch<uint64_t>(source, result, count);
		break;
	case LogicalTypeId::HUGEINT:
		NumericCastSwitch<hugeint_t>(source, result, count);
		break;
	case LogicalTypeId::DECIMAL:
		DecimalCastSwitch(source, result, count);
		break;
	case LogicalTypeId::FLOAT:
		NumericCastSwitch<float>(source, result, count);
		break;
	case LogicalTypeId::DOUBLE:
		NumericCastSwitch<double>(source, result, count);
		break;
	case LogicalTypeId::DATE:
		DateCastSwitch(source, result, count);
		break;
	case LogicalTypeId::TIME:
		TimeCastSwitch(source, result, count);
		break;
	case LogicalTypeId::TIMESTAMP:
		TimestampCastSwitch(source, result, count);
		break;
	case LogicalTypeId::INTERVAL:
		IntervalCastSwitch(source, result, count);
		break;
	case LogicalTypeId::VARCHAR:
		StringCastSwitch(source, result, count, strict);
		break;
	case LogicalTypeId::BLOB:
		BlobCastSwitch(source, result, count);
		break;
	case LogicalTypeId::SQLNULL: {
		// cast a NULL to another type, just copy the properties and change the type
		result.buffer->vector_type = VectorType::CONSTANT_VECTOR;
		ConstantVector::SetNull(result, true);
		break;
	}
	case LogicalTypeId::STRUCT:
		StructCastSwitch(source, result, count);
		break;
	case LogicalTypeId::LIST:
		ListCastSwitch(source, result, count);
		break;
	default:
		throw UnimplementedCast(source.buffer->type, result.buffer->type);
	}
}

} // namespace duckdb
