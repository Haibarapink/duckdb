#include "duckdb/function/compression/compression.hpp"

namespace duckdb {

template<class T>
struct RLEAnalyzeState : public AnalyzeState {
	RLEAnalyzeState() : seen_count(0), last_seen_count(0) {}

	idx_t seen_count;
	T last_value;
	idx_t last_seen_count;
};

template<class T>
unique_ptr<AnalyzeState> RLEInitAnalyze(ColumnData &col_data, PhysicalType type) {
	return make_unique<RLEAnalyzeState<T>>();
}

template<class T>
bool RLEAnalyze(AnalyzeState &state, Vector &input, idx_t count) {
	throw InternalException("FIXME: RLE");
}

template<class T>
idx_t RLEFinalAnalyze(AnalyzeState &state) {
	throw InternalException("FIXME: RLE");
}

template<class T>
unique_ptr<CompressionState> RLEInitCompression(AnalyzeState &state) {
	throw InternalException("FIXME: RLE");
}

template<class T>
idx_t RLECompress(BufferHandle &block, idx_t data_written, Vector& intermediate, idx_t count, CompressionState& state) {
	throw InternalException("FIXME: RLE");
}

template<class T>
idx_t RLEFlush(BufferHandle &block, idx_t data_written, CompressionState& state) {
	throw InternalException("FIXME: RLE");
}

template<class T>
CompressionFunction GetRLEFunction(PhysicalType data_type) {
	return CompressionFunction(
		CompressionType::COMPRESSION_RLE,
		data_type,
		RLEInitAnalyze<T>,
		RLEAnalyze<T>,
		RLEFinalAnalyze<T>,
		RLEInitCompression<T>,
		RLECompress<T>,
		RLEFlush<T>
	);
}

CompressionFunction RLEFun::GetFunction(PhysicalType type) {
	switch(type) {
	case PhysicalType::BOOL:
		return GetRLEFunction<bool>(type);
	case PhysicalType::INT8:
		return GetRLEFunction<int8_t>(type);
	case PhysicalType::INT16:
		return GetRLEFunction<int16_t>(type);
	case PhysicalType::INT32:
		return GetRLEFunction<int32_t>(type);
	case PhysicalType::INT64:
		return GetRLEFunction<int64_t>(type);
	case PhysicalType::INT128:
		return GetRLEFunction<hugeint_t>(type);
	case PhysicalType::UINT8:
		return GetRLEFunction<uint8_t>(type);
	case PhysicalType::UINT16:
		return GetRLEFunction<uint16_t>(type);
	case PhysicalType::UINT32:
		return GetRLEFunction<uint32_t>(type);
	case PhysicalType::UINT64:
		return GetRLEFunction<uint64_t>(type);
	case PhysicalType::FLOAT:
		return GetRLEFunction<float>(type);
	case PhysicalType::DOUBLE:
		return GetRLEFunction<double>(type);
	default:
		throw InternalException("Unsupported type for RLE");
	}
}

bool RLEFun::TypeIsSupported(PhysicalType type) {
	switch(type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		return true;
	default:
		return false;
	}
}

}
