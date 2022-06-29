#include "duckdb_python/vector_conversion.hpp"
#include "duckdb_python/python_conversion.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb_python/python_import_cache.hpp"

namespace duckdb {

template <class T>
void ScanPandasColumn(py::array &numpy_col, idx_t stride, idx_t offset, Vector &out, idx_t count) {
	auto src_ptr = (T *)numpy_col.data();
	if (stride == sizeof(T)) {
		FlatVector::SetData(out, (data_ptr_t)(src_ptr + offset));
	} else {
		auto tgt_ptr = (T *)FlatVector::GetData(out);
		for (idx_t i = 0; i < count; i++) {
			tgt_ptr[i] = src_ptr[stride / sizeof(T) * (i + offset)];
		}
	}
}

template <class T, class V>
void ScanPandasCategoryTemplated(py::array &column, idx_t offset, Vector &out, idx_t count) {
	auto src_ptr = (T *)column.data();
	auto tgt_ptr = (V *)FlatVector::GetData(out);
	auto &tgt_mask = FlatVector::Validity(out);
	for (idx_t i = 0; i < count; i++) {
		if (src_ptr[i + offset] == -1) {
			// Null value
			tgt_mask.SetInvalid(i);
		} else {
			tgt_ptr[i] = src_ptr[i + offset];
		}
	}
}

template <class T>
void ScanPandasCategory(py::array &column, idx_t count, idx_t offset, Vector &out, string &src_type) {
	if (src_type == "int8") {
		ScanPandasCategoryTemplated<int8_t, T>(column, offset, out, count);
	} else if (src_type == "int16") {
		ScanPandasCategoryTemplated<int16_t, T>(column, offset, out, count);
	} else if (src_type == "int32") {
		ScanPandasCategoryTemplated<int32_t, T>(column, offset, out, count);
	} else {
		throw NotImplementedException("The Pandas type " + src_type + " for categorical types is not implemented yet");
	}
}

template <class T>
void ScanPandasMasked(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
	ScanPandasColumn<T>(bind_data.numpy_col, bind_data.numpy_stride, offset, out, count);
	auto &result_mask = FlatVector::Validity(out);
	if (bind_data.mask) {
		auto mask = (bool *)bind_data.mask->numpy_array.data();
		for (idx_t i = 0; i < count; i++) {
			auto is_null = mask[offset + i];
			if (is_null) {
				result_mask.SetInvalid(i);
			}
		}
	}
}

template <class T>
bool ValueIsNull(T value) {
	throw std::runtime_error("unsupported type for ValueIsNull");
}

template <>
bool ValueIsNull(float value) {
	return !Value::FloatIsFinite(value);
}

template <>
bool ValueIsNull(double value) {
	return !Value::DoubleIsFinite(value);
}

template <class T>
void ScanPandasFpColumn(T *src_ptr, idx_t count, idx_t offset, Vector &out) {
	FlatVector::SetData(out, (data_ptr_t)(src_ptr + offset));
	auto tgt_ptr = FlatVector::GetData<T>(out);
	auto &mask = FlatVector::Validity(out);
	for (idx_t i = 0; i < count; i++) {
		if (ValueIsNull(tgt_ptr[i])) {
			mask.SetInvalid(i);
		}
	}
}

template <class T>
static string_t DecodePythonUnicode(T *codepoints, idx_t codepoint_count, Vector &out) {
	// first figure out how many bytes to allocate
	idx_t utf8_length = 0;
	for (idx_t i = 0; i < codepoint_count; i++) {
		int len = Utf8Proc::CodepointLength(int(codepoints[i]));
		D_ASSERT(len >= 1);
		utf8_length += len;
	}
	int sz;
	auto result = StringVector::EmptyString(out, utf8_length);
	auto target = result.GetDataWriteable();
	for (idx_t i = 0; i < codepoint_count; i++) {
		Utf8Proc::CodepointToUtf8(int(codepoints[i]), sz, target);
		D_ASSERT(sz >= 1);
		target += sz;
	}
	result.Finalize();
	return result;
}

template <typename T>
bool TryCast(const py::object stuf, T &value) {
	try {
		value = stuf.cast<T>();
		return true;
	} catch (py::cast_error) {
		return false;
	}
}

template <typename T>
T Cast(const py::object obj) {
	return obj.cast<T>();
}

static LogicalType ConvertPandasType(const PandasType &col_type) {
	switch (col_type) {
	case PandasType::BOOL:
	case PandasType::PANDA_BOOL: {
		return LogicalType::BOOLEAN;
	}
	case PandasType::PANDA_INT8:
	case PandasType::INT_8: {
		return LogicalType::TINYINT;
	}
	case PandasType::PANDA_UINT8:
	case PandasType::UINT_8: {
		return LogicalType::UTINYINT;
	}
	case PandasType::PANDA_INT16:
	case PandasType::INT_16: {
		return LogicalType::SMALLINT;
	}
	case PandasType::PANDA_UINT16:
	case PandasType::UINT_16: {
		return LogicalType::USMALLINT;
	}
	case PandasType::PANDA_INT32:
	case PandasType::INT_32: {
		return LogicalType::INTEGER;
	}
	case PandasType::PANDA_UINT32:
	case PandasType::UINT_32: {
		return LogicalType::UINTEGER;
	}
	case PandasType::PANDA_INT64:
	case PandasType::INT_64: {
		return LogicalType::BIGINT;
	}
	case PandasType::PANDA_UINT64:
	case PandasType::UINT_64: {
		return LogicalType::UBIGINT;
	}
	case PandasType::PANDA_FLOAT32:
	case PandasType::FLOAT_32: {
		return LogicalType::FLOAT;
	}
	case PandasType::PANDA_FLOAT64:
	case PandasType::LONG_FLOAT_64:
	case PandasType::FLOAT_64: {
		return LogicalType::DOUBLE;
	}
	case PandasType::OBJECT: {
		return LogicalType::VARCHAR;
	}
	case PandasType::PANDA_STRING: {
		return LogicalType::VARCHAR;
	}
	case PandasType::TIMEDELTA: {
		return LogicalType::INTERVAL;
	}
	case PandasType::PANDA_DATETIME:
	case PandasType::DATETIME: {
		return LogicalType::TIMESTAMP;
	}
	default: {
		throw std::runtime_error("Failed to convert dtype num " + to_string((uint8_t)col_type) +
		                         " to duckdb LogicalType");
	}
	}
}

template <class T>
PandasType GetPandasType(T dtype) {
	int64_t extended_type;

	if (py::hasattr(dtype, "num")) {
		extended_type = py::int_(dtype.attr("num"));
	} else {
		auto type_str = string(py::str(dtype));
		if (type_str == "boolean") {
			return PandasType::PANDA_BOOL;
		} else if (type_str == "category") {
			return PandasType::PANDA_CATEGORY;
		} else if (type_str == "Int8") {
			return PandasType::PANDA_INT8;
		} else if (type_str == "Int16") {
			return PandasType::PANDA_INT16;
		} else if (type_str == "Int32") {
			return PandasType::PANDA_INT32;
		} else if (type_str == "Int64") {
			return PandasType::PANDA_INT64;
		} else if (type_str == "UInt8") {
			return PandasType::PANDA_UINT8;
		} else if (type_str == "UInt16") {
			return PandasType::PANDA_UINT16;
		} else if (type_str == "UInt32") {
			return PandasType::PANDA_UINT32;
		} else if (type_str == "UInt64") {
			return PandasType::PANDA_UINT64;
		} else if (type_str == "Float32") {
			return PandasType::PANDA_FLOAT32;
		} else if (type_str == "Float64") {
			return PandasType::PANDA_FLOAT64;
		} else if (type_str == "string") {
			return PandasType::PANDA_STRING;
		} else {
			throw std::runtime_error("Unknown dtype (" + type_str + ")");
		}
	}
	// 100 (PANDA_EXTENSION_TYPE) is potentially used by multiple dtypes, need to figure out which one it is exactly.
	if (extended_type == (int64_t)PandasType::PANDA_EXTENSION_TYPE) {
		auto extension_type_str = string(py::str(dtype));
		if (extension_type_str == "category") {
			return PandasType::PANDA_CATEGORY;
		} else {
			throw std::runtime_error("Unknown extension dtype (" + extension_type_str + ")");
		}
	}
	// Since people can extend the dtypes with their own custom stuff, it's probably best to check if it falls out of
	// the supported range of dtypes. (little hardcoded though)
	if (!(extended_type >= 0 && extended_type <= 23) && !(extended_type >= 100 && extended_type <= 103)) {
		throw std::runtime_error("Dtype num " + to_string(extended_type) + " is not supported");
	}
	return (PandasType)extended_type;
}

//! 'count' is the amount of rows in the 'out' vector
//! offset is the current row number within this vector
void ScanPandasObject(PandasColumnBindData &bind_data, PyObject *object, idx_t offset, Vector &out) {

	// handle None
	if (object == Py_None) {
		auto &validity = FlatVector::Validity(out);
		validity.SetInvalid(offset);
		return;
	}

	auto val = TransformPythonValue(object);
	// Check if the Value type is accepted for the LogicalType of Vector
	out.SetValue(offset, val);
}

void ScanPandasObjectColumn(PandasColumnBindData &bind_data, PyObject **col, idx_t count, idx_t offset, Vector &out) {
	// numpy_col is a sequential list of objects, that make up one "column" (Vector)
	out.SetVectorType(VectorType::FLAT_VECTOR);
	auto gil = make_unique<PythonGILWrapper>(); // We're creating python objects here, so we need the GIL

	for (idx_t i = 0; i < count; i++) {
		ScanPandasObject(bind_data, col[i], i, out);
	}
}

void VectorConversion::NumpyToDuckDB(PandasColumnBindData &bind_data, py::array &numpy_col, idx_t count, idx_t offset,
                                     Vector &out) {
	switch (bind_data.pandas_type) {
	case PandasType::PANDA_BOOL:
		ScanPandasMasked<bool>(bind_data, count, offset, out);
		break;
	case PandasType::BOOL:
		ScanPandasColumn<bool>(numpy_col, bind_data.numpy_stride, offset, out, count);
		break;
	case PandasType::UINT_8:
	case PandasType::PANDA_UINT8:
		ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
		break;
	case PandasType::UINT_16:
	case PandasType::PANDA_UINT16:
		ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
		break;
	case PandasType::UINT_32:
	case PandasType::PANDA_UINT32:
		ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
		break;
	case PandasType::UINT_64:
	case PandasType::PANDA_UINT64:
		ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
		break;
	case PandasType::INT_8:
	case PandasType::PANDA_INT8:
		ScanPandasMasked<int8_t>(bind_data, count, offset, out);
		break;
	case PandasType::INT_16:
	case PandasType::PANDA_INT16:
		ScanPandasMasked<int16_t>(bind_data, count, offset, out);
		break;
	case PandasType::INT_32:
	case PandasType::PANDA_INT32:
		ScanPandasMasked<int32_t>(bind_data, count, offset, out);
		break;
	case PandasType::INT_64:
	case PandasType::PANDA_INT64:
		ScanPandasMasked<int64_t>(bind_data, count, offset, out);
		break;
	case PandasType::FLOAT_32:
	case PandasType::PANDA_FLOAT32:
		ScanPandasFpColumn<float>((float *)numpy_col.data(), count, offset, out);
		break;
	case PandasType::FLOAT_64:
	case PandasType::LONG_FLOAT_64:
	case PandasType::PANDA_FLOAT64:
		ScanPandasFpColumn<double>((double *)numpy_col.data(), count, offset, out);
		break;
	case PandasType::PANDA_DATETIME:
	case PandasType::DATETIME: {
		auto src_ptr = (int64_t *)numpy_col.data();
		auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
		auto &mask = FlatVector::Validity(out);

		for (idx_t row = 0; row < count; row++) {
			auto source_idx = offset + row;
			if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
				// pandas Not a Time (NaT)
				mask.SetInvalid(row);
				continue;
			}
			tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
		}
		break;
	}
	case PandasType::TIMEDELTA:
	case PandasType::PANDA_INTERVAL: {
		auto src_ptr = (int64_t *)numpy_col.data();
		auto tgt_ptr = FlatVector::GetData<interval_t>(out);
		auto &mask = FlatVector::Validity(out);

		for (idx_t row = 0; row < count; row++) {
			auto source_idx = offset + row;
			if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
				// pandas Not a Time (NaT)
				mask.SetInvalid(row);
				continue;
			}
			int64_t micro = src_ptr[source_idx] / 1000;
			int64_t days = micro / Interval::MICROS_PER_DAY;
			micro = micro % Interval::MICROS_PER_DAY;
			int64_t months = days / Interval::DAYS_PER_MONTH;
			days = days % Interval::DAYS_PER_MONTH;
			interval_t interval;
			interval.months = months;
			interval.days = days;
			interval.micros = micro;
			tgt_ptr[row] = interval;
		}
		break;
	}
	case PandasType::PANDA_STRING:
	case PandasType::OBJECT: {
		//! We have determined the underlying logical type of this object column
		// Get the source pointer of the numpy array
		auto src_ptr = (PyObject **)numpy_col.data();
		if (out.GetType().id() != LogicalTypeId::VARCHAR) {
			return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
		}

		if (out.GetType().id() != LogicalTypeId::VARCHAR) {
			return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
		}

		// Get the data pointer and the validity mask of the result vector
		auto tgt_ptr = FlatVector::GetData<string_t>(out);
		auto &out_mask = FlatVector::Validity(out);
		unique_ptr<PythonGILWrapper> gil;

		// Loop over every row of the arrays contents
		for (idx_t row = 0; row < count; row++) {
			auto source_idx = offset + row;

			// Get the pointer to the object
			PyObject *val = src_ptr[source_idx];
			if (bind_data.pandas_type == PandasType::OBJECT && !PyUnicode_CheckExact(val)) {
				LogicalType ltype = out.GetType();
				if (val == Py_None) {
					out_mask.SetInvalid(row);
					continue;
				}
				if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
					out_mask.SetInvalid(row);
					continue;
				}
				if (!py::isinstance<py::str>(val)) {
					if (!gil) {
						gil = bind_data.object_str_val.GetLock();
					}
					bind_data.object_str_val.AssignInternal<PyObject>(
					    [](py::str &obj, PyObject &new_val) {
						    py::handle object_handle = &new_val;
						    obj = py::str(object_handle);
					    },
					    *val, *gil);
					val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
				}
			}
			// Python 3 string representation:
			// https://github.com/python/cpython/blob/3a8fdb28794b2f19f6c8464378fb8b46bce1f5f4/Include/cpython/unicodeobject.h#L79
			if (!PyUnicode_CheckExact(val)) {
				out_mask.SetInvalid(row);
				continue;
			}
			if (PyUnicode_IS_COMPACT_ASCII(val)) {
				// ascii string: we can zero copy
				tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
			} else {
				// unicode gunk
				auto ascii_obj = (PyASCIIObject *)val;
				auto unicode_obj = (PyCompactUnicodeObject *)val;
				// compact unicode string: is there utf8 data available?
				if (unicode_obj->utf8) {
					// there is! zero copy
					tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
				} else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
					auto kind = PyUnicode_KIND(val);
					switch (kind) {
					case PyUnicode_1BYTE_KIND:
						tgt_ptr[row] =
						    DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
						break;
					case PyUnicode_2BYTE_KIND:
						tgt_ptr[row] =
						    DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
						break;
					case PyUnicode_4BYTE_KIND:
						tgt_ptr[row] =
						    DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
						break;
					default:
						throw std::runtime_error("Unsupported typekind for Python Unicode Compact decode");
					}
				} else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
					throw std::runtime_error("Unsupported: decode not ready legacy string");
				} else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
					throw std::runtime_error("Unsupported: decode ready legacy string");
				} else {
					throw std::runtime_error("Unsupported string type: no clue what this string is");
				}
			}
		}
		break;
	}
	case PandasType::PANDA_CATEGORY: {
		switch (out.GetType().InternalType()) {
		case PhysicalType::UINT8:
			ScanPandasCategory<uint8_t>(numpy_col, count, offset, out, bind_data.internal_categorical_type);
			break;
		case PhysicalType::UINT16:
			ScanPandasCategory<uint16_t>(numpy_col, count, offset, out, bind_data.internal_categorical_type);
			break;
		case PhysicalType::UINT32:
			ScanPandasCategory<uint32_t>(numpy_col, count, offset, out, bind_data.internal_categorical_type);
			break;
		default:
			throw InternalException("Invalid Physical Type for ENUMs");
		}
		break;
	}

	default:
		throw std::runtime_error("Unsupported dtype num " + to_string((uint8_t)bind_data.pandas_type));
	}
}

static py::object GetItem(py::handle &column, idx_t index) {
	return column.attr("__getitem__")(index);
}

static duckdb::LogicalType GetItemType(py::handle &ele, bool &can_convert);

static duckdb::LogicalType GetListType(py::handle &ele, bool &can_convert) {
	auto size = py::len(ele);

	if (size == 0) {
		return LogicalType::LIST(LogicalType::SQLNULL);
	}

	idx_t i = 0;
	LogicalType list_type = LogicalType::SQLNULL;
	for (auto py_val : ele) {
		auto item_type = GetItemType(py_val, can_convert);
		if (!i) {
			list_type = item_type;
		} else {
			list_type = LogicalType::MaxLogicalType(list_type, item_type);
		}
		if (!can_convert) {
			break;
		}
		i++;
	}
	return LogicalType::LIST(list_type);
}

static duckdb::LogicalType EmptyMap() {
	child_list_t<LogicalType> child_types;
	auto empty = LogicalType::LIST(LogicalTypeId::SQLNULL);
	child_types.push_back(make_pair("key", empty));
	child_types.push_back(make_pair("value", empty));
	return LogicalType::MAP(move(child_types));
}

//! Check if the keys match
static bool StructKeysAreEqual(idx_t row, const child_list_t<LogicalType> &reference,
                               const child_list_t<LogicalType> &compare, string &error_message) {
	D_ASSERT(reference.size() == compare.size());
	for (idx_t i = 0; i < reference.size(); i++) {
		auto &ref = reference[i].first;
		auto &comp = compare[i].first;
		if (!duckdb::CaseInsensitiveStringEquality()(ref, comp)) {
			error_message = "Struct key on row " + to_string(row) + " is incorrect, expected '" + ref +
			                "' but encountered '" + comp + "'";
			return false;
		}
	}
	return true;
}

// Verify that all struct entries in a column have the same amount of fields and that keys are equal
static void VerifyStructValidity(vector<LogicalType> &structs) {
	D_ASSERT(!structs.empty());
	idx_t reference_entry = 0;
	// Get first non-null entry
	for (; reference_entry < structs.size(); reference_entry++) {
		if (structs[reference_entry].id() != LogicalTypeId::SQLNULL) {
			break;
		}
	}
	// All entries are NULL
	if (reference_entry == structs.size()) {
		return;
	}
	auto reference_type = structs[0];
	auto reference_children = StructType::GetChildTypes(reference_type);

	string error_message;
	for (idx_t i = reference_entry + 1; i < structs.size(); i++) {
		auto &entry = structs[i];
		if (entry.id() == LogicalTypeId::SQLNULL) {
			continue;
		}
		auto &entry_children = StructType::GetChildTypes(entry);
		if (entry_children.size() != reference_children.size()) {
			throw std::runtime_error("Struct entries have differing amounts of fields");
		}
		if (!StructKeysAreEqual(i, reference_children, entry_children, error_message)) {
			throw std::runtime_error(error_message);
		}
	}
}

static duckdb::LogicalType AnalyzeObjectType(py::handle column, bool &can_convert) {
	idx_t rows = py::len(column);
	LogicalType item_type = duckdb::LogicalType::SQLNULL;
	if (!rows) {
		return item_type;
	}

	// Keys are not guaranteed to start at 0 for Series, use the internal __array__ instead
	auto pandas_module = py::module::import("pandas");
	auto pandas_series = pandas_module.attr("core").attr("series").attr("Series");
	if (py::isinstance(column, pandas_series)) {
		column = column.attr("__array__")();
	}

	vector<LogicalType> types;
	auto first_item = GetItem(column, 0);
	item_type = GetItemType(first_item, can_convert);
	if (!can_convert) {
		return item_type;
	}
	types.push_back(item_type);

	for (idx_t i = 1; i < rows; i++) {
		auto next_item = GetItem(column, i);
		auto next_item_type = GetItemType(next_item, can_convert);
		types.push_back(next_item_type);
		if (!can_convert) {
			break;
		}
		item_type = LogicalType::MaxLogicalType(item_type, next_item_type);
	}
	if (item_type.id() == LogicalTypeId::STRUCT) {
		VerifyStructValidity(types);
	}

	return item_type;
}

static duckdb::LogicalType DictToMap(py::handle &dict_values, bool &can_convert) {
	auto keys = dict_values.attr("__getitem__")(0);
	auto values = dict_values.attr("__getitem__")(1);

	child_list_t<LogicalType> child_types;
	auto key_type = GetListType(keys, can_convert);
	if (!can_convert) {
		return EmptyMap();
	}
	auto value_type = GetListType(values, can_convert);
	if (!can_convert) {
		return EmptyMap();
	}

	child_types.push_back(make_pair("key", key_type));
	child_types.push_back(make_pair("value", value_type));
	return LogicalType::MAP(move(child_types));
}

//! Python dictionaries don't allow duplicate keys, so we don't need to check this.
static duckdb::LogicalType DictToStruct(py::handle &dict_keys, py::handle &dict_values, idx_t size, bool &can_convert) {
	child_list_t<LogicalType> struct_children;

	for (idx_t i = 0; i < size; i++) {
		auto dict_key = dict_keys.attr("__getitem__")(i);

		//! Have to already transform here because the child_list needs a string as key
		auto key = string(py::str(dict_key));

		auto dict_val = dict_values.attr("__getitem__")(i);
		auto val = GetItemType(dict_val, can_convert);
		struct_children.push_back(make_pair(key, move(val)));
	}
	return LogicalType::STRUCT(move(struct_children));
}

//! 'can_convert' is used to communicate if internal structures encountered here are valid
//! e.g python lists can consist of multiple different types, which we cant communicate downwards through
//! LogicalType's alone

static duckdb::LogicalType GetItemType(py::handle &ele, bool &can_convert) {
	PythonImportCache import_cache;

	if (ele.is_none()) {
		return LogicalType::SQLNULL;
	} else if (py::isinstance<py::bool_>(ele)) {
		return LogicalType::BOOLEAN;
	} else if (py::isinstance<py::int_>(ele)) {
		return LogicalType::BIGINT;
	} else if (py::isinstance<py::float_>(ele)) {
		if (std::isnan(PyFloat_AsDouble(ele.ptr()))) {
			return LogicalType::SQLNULL;
		}
		return LogicalType::DOUBLE;
	} else if (py::isinstance(ele, import_cache.decimal.Decimal())) {
		return LogicalType::VARCHAR; // Might be float64 actually? //or DECIMAL
	} else if (py::isinstance(ele, import_cache.datetime.datetime())) {
		// auto ptr = ele.ptr();
		// auto year = PyDateTime_GET_YEAR(ptr);
		// auto month = PyDateTime_GET_MONTH(ptr);
		// auto day = PyDateTime_GET_DAY(ptr);
		// auto hour = PyDateTime_DATE_GET_HOUR(ptr);
		// auto minute = PyDateTime_DATE_GET_MINUTE(ptr);
		// auto second = PyDateTime_DATE_GET_SECOND(ptr);
		// auto micros = PyDateTime_DATE_GET_MICROSECOND(ptr);
		// This probably needs to be more precise ..
		return LogicalType::TIMESTAMP;
	} else if (py::isinstance(ele, import_cache.datetime.time())) {
		// auto ptr = ele.ptr();
		// auto hour = PyDateTime_TIME_GET_HOUR(ptr);
		// auto minute = PyDateTime_TIME_GET_MINUTE(ptr);
		// auto second = PyDateTime_TIME_GET_SECOND(ptr);
		// auto micros = PyDateTime_TIME_GET_MICROSECOND(ptr);
		return LogicalType::TIME;
	} else if (py::isinstance(ele, import_cache.datetime.date())) {
		// auto ptr = ele.ptr();
		// auto year = PyDateTime_GET_YEAR(ptr);
		// auto month = PyDateTime_GET_MONTH(ptr);
		// auto day = PyDateTime_GET_DAY(ptr);
		return LogicalType::DATE;
	} else if (py::isinstance<py::str>(ele)) {
		return LogicalType::VARCHAR;
	} else if (py::isinstance(ele, import_cache.uuid.UUID())) {
		return LogicalType::UUID;
	} else if (py::isinstance<py::bytearray>(ele)) {
		return LogicalType::BLOB;
	} else if (py::isinstance<py::memoryview>(ele)) {
		return LogicalType::BLOB;
	} else if (py::isinstance<py::bytes>(ele)) {
		return LogicalType::BLOB;
	} else if (py::isinstance<py::list>(ele)) {
		return GetListType(ele, can_convert);
	} else if (py::isinstance<py::dict>(ele)) {
		auto dict_keys = py::list(ele.attr("keys")());
		auto dict_values = py::list(ele.attr("values")());
		auto size = py::len(dict_values);
		// Assuming keys and values are the same size

		if (size == 0) {
			return EmptyMap();
		}
		if (DictionaryHasMapFormat(dict_values, size)) {
			return DictToMap(dict_values, can_convert);
		}
		return DictToStruct(dict_keys, dict_values, size, can_convert);
	} else if (py::isinstance(ele, import_cache.numpy.ndarray())) {
		auto extended_type = GetPandasType(ele.attr("dtype"));
		LogicalType ltype;
		ltype = ConvertPandasType(extended_type);
		if (extended_type == PandasType::OBJECT) {
			LogicalType converted_type = AnalyzeObjectType(ele, can_convert);
			if (can_convert) {
				ltype = converted_type;
			}
		}
		return LogicalType::LIST(ltype);
	} else {
		// Fall back to string for unknown types
		can_convert = false;
		return LogicalType::VARCHAR;
	}
}

bool ColumnIsMasked(pybind11::detail::accessor<pybind11::detail::accessor_policies::list_item> column,
                    const PandasType &type) {
	bool masked = py::hasattr(column, "mask");
	return (masked || type == PandasType::PANDA_INT8 || type == PandasType::PANDA_INT16 ||
	        type == PandasType::PANDA_INT32 || type == PandasType::PANDA_INT64 || type == PandasType::PANDA_UINT8 ||
	        type == PandasType::PANDA_UINT16 || type == PandasType::PANDA_UINT32 || type == PandasType::PANDA_UINT64 ||
	        type == PandasType::PANDA_FLOAT32 || type == PandasType::PANDA_FLOAT64 || type == PandasType::PANDA_BOOL);
}

void VectorConversion::BindPandas(py::handle df, vector<PandasColumnBindData> &bind_columns,
                                  vector<LogicalType> &return_types, vector<string> &names) {
	// This performs a shallow copy that allows us to rename the dataframe
	auto df_columns = py::list(df.attr("columns"));
	auto df_types = py::list(df.attr("dtypes"));
	auto get_fun = df.attr("__getitem__");
	// TODO support masked arrays as well
	// TODO support dicts of numpy arrays as well
	if (py::len(df_columns) == 0 || py::len(df_types) == 0 || py::len(df_columns) != py::len(df_types)) {
		throw std::runtime_error("Need a DataFrame with at least one column");
	}
	py::array column_attributes = df.attr("columns").attr("values");

	// loop over every column
	for (idx_t col_idx = 0; col_idx < py::len(df_columns); col_idx++) {
		LogicalType duckdb_col_type;
		PandasColumnBindData bind_data;

		names.emplace_back(py::str(df_columns[col_idx]));
		auto dtype = GetPandasType(df_types[col_idx]);
		bool masked = ColumnIsMasked(df_columns[col_idx], dtype);
		// auto col_type = string(py::str(df_types[col_idx]));

		bind_data.mask = nullptr;
		if (masked) {
			// masked object
			// fetch the internal data and mask array
			bind_data.mask = make_unique<NumPyArrayWrapper>(get_fun(df_columns[col_idx]).attr("array").attr("_mask"));
		}

		auto column = get_fun(df_columns[col_idx]);
		if (dtype == PandasType::PANDA_CATEGORY) {
			// for category types, we create an ENUM type for string or use the converted numpy type for the rest
			D_ASSERT(py::hasattr(column, "cat"));
			D_ASSERT(py::hasattr(column.attr("cat"), "categories"));
			auto categories = py::array(column.attr("cat").attr("categories"));

			auto category_type = GetPandasType(categories.attr("dtype"));
			if (category_type == PandasType::OBJECT) {
				// Let's hope the object type is a string.
				bind_data.pandas_type = PandasType::PANDA_CATEGORY;
				auto enum_name = string(py::str(df_columns[col_idx]));
				vector<string> enum_entries = py::cast<vector<string>>(categories);
				idx_t size = enum_entries.size();
				Vector enum_entries_vec(LogicalType::VARCHAR, size);
				auto enum_entries_ptr = FlatVector::GetData<string_t>(enum_entries_vec);
				for (idx_t i = 0; i < size; i++) {
					enum_entries_ptr[i] = StringVector::AddStringOrBlob(enum_entries_vec, enum_entries[i]);
				}
				D_ASSERT(py::hasattr(column.attr("cat"), "codes"));
				duckdb_col_type = LogicalType::ENUM(enum_name, enum_entries_vec, size);
				bind_data.numpy_col = py::array(column.attr("cat").attr("codes"));
				D_ASSERT(py::hasattr(bind_data.numpy_col, "dtype"));
				bind_data.internal_categorical_type = string(py::str(bind_data.numpy_col.attr("dtype")));
			} else {
				bind_data.numpy_col = py::array(column.attr("to_numpy")());
				auto numpy_type = bind_data.numpy_col.attr("dtype");
				// for category types (non-strings), we use the converted numpy type
				category_type = GetPandasType(numpy_type);
				bind_data.pandas_type = category_type;
				duckdb_col_type = ConvertPandasType(category_type);
			}
		} else {
			if (masked || dtype == PandasType::DATETIME || dtype == PandasType::PANDA_DATETIME) {
				bind_data.numpy_col = get_fun(df_columns[col_idx]).attr("array").attr("_data");
			} else {
				bind_data.numpy_col = py::array(column.attr("to_numpy")());
			}
			bind_data.pandas_type = dtype;
			duckdb_col_type = ConvertPandasType(dtype);
		}

		// Analyze the inner data type of the 'object' column
		if (bind_data.pandas_type == PandasType::OBJECT) {
			bool can_convert = true;
			py::gil_scoped_acquire acquire;
			LogicalType converted_type = AnalyzeObjectType(get_fun(df_columns[col_idx]), can_convert);
			if (can_convert) {
				duckdb_col_type = converted_type;
			}
		}

		D_ASSERT(py::hasattr(bind_data.numpy_col, "strides"));
		bind_data.numpy_stride = bind_data.numpy_col.attr("strides").attr("__getitem__")(0).cast<idx_t>();
		return_types.push_back(duckdb_col_type);
		bind_columns.push_back(move(bind_data));
	}
}

//'?'
//
// boolean
//
//'b'
//
//(signed) byte
//
//'B'
//
// unsigned byte
//
//'i'
//
//(signed) integer
//
//'u'
//
// unsigned integer
//
//'f'
//
// floating-point
//
//'c'
//
// complex-floating point
//
//'m'
//
// timedelta
//
//'M'
//
// datetime
//
//'O'
//
//(Python) objects
//
//'S', 'a'
//
// zero-terminated bytes (not recommended)
//
//'U'
//
// Unicode string
//
//'V'
//
// raw data (void)
// test

static string GetItemDataType(idx_t index, pybind11::object &col) {
	return py::str(col.attr("__getitem__")(index).get_type().attr("__name__"));
}

static bool MeetsConversionRequirements(const string &type, pybind11::object &column, idx_t rows) {

	if (type == "object") {
		// Can't change
		return false;
	}
	for (size_t i = 1; i < rows; i++) {
		string other = GetItemDataType(i, column);
		if (type != other) {
			// Not all the same type
			return false;
		}
	}
	return true;
}

static void ConvertSingleColumn(py::handle df, idx_t col_idx) {
	auto df_columns = py::list(df.attr("columns"));
	auto df_types = py::list(df.attr("dtypes"));
	auto get_fun = df.attr("__getitem__");
	auto set_fun = df.attr("__setitem__");

	// We gotta check if all the types of the column are the same, if yes we can change it
	auto column = get_fun(df_columns[col_idx]);

	idx_t rows = py::len(column);
	D_ASSERT(rows > 0);
	string column_type = GetItemDataType(0, column);
	if (!MeetsConversionRequirements(column_type, column, rows)) {
		return;
	}

	if (column_type == "date") {
		set_fun(df_columns[col_idx], column.attr("astype")("M", py::arg("copy") = false));
	}
	// pandas uses native python strings, which have to stay 'object'
	else if (column_type == "str") {
		return;
	} else if (column_type == "int") {
		set_fun(df_columns[col_idx], column.attr("astype")("i", py::arg("copy") = false));
	} else if (column_type == "bool") {
		set_fun(df_columns[col_idx], column.attr("astype")("?", py::arg("copy") = false));
	} else if (column_type == "float") {
		set_fun(df_columns[col_idx], column.attr("astype")("f", py::arg("copy") = false));
	} else if (column_type == "clongdouble") {
		set_fun(df_columns[col_idx], column.attr("astype")("G", py::arg("copy") = false));
	} else if (column_type == "longdouble") {
		set_fun(df_columns[col_idx], column.attr("astype")("g", py::arg("copy") = false));
	} else if (column_type == "complex") {
		set_fun(df_columns[col_idx], column.attr("astype")("D", py::arg("copy") = false));
	} else if (column_type == "bytes") {
		// These are essentially just fixed-size strings, so it's fine if they stay object
		return;
	} else if (column_type == "timedelta") {
		set_fun(df_columns[col_idx], column.attr("astype")("m", py::arg("copy") = false));
	} else {
		throw std::runtime_error(column_type);
	}
}

void VectorConversion::Analyze(py::handle df) {
	auto df_columns = py::list(df.attr("columns"));
	auto df_types = py::list(df.attr("dtypes"));
	auto empty = py::bool_(df.attr("empty"));
	auto get_fun = df.attr("__getitem__");
	auto set_fun = df.attr("__setitem__");

	if (empty) {
		return;
	}

	if (py::len(df_columns) == 0 || py::len(df_types) == 0 || py::len(df_columns) != py::len(df_types)) {
		throw std::runtime_error("Need a DataFrame with at least one column");
	}
	for (idx_t col_idx = 0; col_idx < py::len(df_columns); col_idx++) {
		auto col_type = string(py::str(df_types[col_idx]));
		if (col_type != "object") {
			continue;
		}
		ConvertSingleColumn(df, col_idx);
	}
}
} // namespace duckdb
