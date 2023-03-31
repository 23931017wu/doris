// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "jni_connector.h"

#include <sstream>

namespace doris::vectorized {

#define FOR_LOGICAL_NUMERIC_TYPES(M) \
    M(TypeIndex::Int8, Int8)         \
    M(TypeIndex::UInt8, UInt8)       \
    M(TypeIndex::Int16, Int16)       \
    M(TypeIndex::UInt16, UInt16)     \
    M(TypeIndex::Int32, Int32)       \
    M(TypeIndex::UInt32, UInt32)     \
    M(TypeIndex::Int64, Int64)       \
    M(TypeIndex::UInt64, UInt64)     \
    M(TypeIndex::Float32, Float32)   \
    M(TypeIndex::Float64, Float64)

JniConnector::~JniConnector() {
    Status st = close();
    if (!st.ok()) {
        // Ensure successful resource release
        LOG(FATAL) << "Failed to release jni resource: " << st.to_string();
    }
}

Status JniConnector::open(RuntimeState* state, RuntimeProfile* profile) {
    RETURN_IF_ERROR(JniUtil::GetJNIEnv(&_env));
    if (_env == nullptr) {
        return Status::InternalError("Failed to get/create JVM");
    }
    RETURN_IF_ERROR(_init_jni_scanner(_env, state->batch_size()));
    // Call org.apache.doris.jni.JniScanner#open
    _env->CallVoidMethod(_jni_scanner_obj, _jni_scanner_open);
    RETURN_ERROR_IF_EXC(_env);
    return Status::OK();
}

Status JniConnector::init(
        std::unordered_map<std::string, ColumnValueRangeType>* colname_to_value_range) {
    _generate_predicates(colname_to_value_range);
    if (_predicates_length != 0 && _predicates != nullptr) {
        int64_t predicates_address = (int64_t)_predicates.get();
        // We can call org.apache.doris.jni.vec.ScanPredicate#parseScanPredicates to parse the
        // serialized predicates in java side.
        _scanner_params.emplace("push_down_predicates", std::to_string(predicates_address));
    }
    return Status::OK();
}

Status JniConnector::get_nex_block(Block* block, size_t* read_rows, bool* eof) {
    JniLocalFrame jni_frame;
    RETURN_IF_ERROR(jni_frame.push(_env));
    // Call org.apache.doris.jni.JniScanner#getNextBatchMeta
    // return the address of meta information
    long meta_address = _env->CallLongMethod(_jni_scanner_obj, _jni_scanner_get_next_batch);
    RETURN_ERROR_IF_EXC(_env);
    if (meta_address == 0) {
        // Address == 0 when there's no data in scanner
        *read_rows = 0;
        *eof = true;
        return Status::OK();
    }
    _set_meta(meta_address);
    long num_rows = _next_meta_as_long();
    if (num_rows == 0) {
        *read_rows = 0;
        *eof = true;
        return Status::OK();
    }
    RETURN_IF_ERROR(_fill_block(block, num_rows));
    *read_rows = num_rows;
    *eof = false;
    _env->CallVoidMethod(_jni_scanner_obj, _jni_scanner_release_table);
    RETURN_ERROR_IF_EXC(_env);
    _has_read += num_rows;
    return Status::OK();
}

Status JniConnector::close() {
    if (!_closed) {
        // _fill_block may be failed and returned, we should release table in close.
        // org.apache.doris.jni.JniScanner#releaseTable is idempotent
        _env->CallVoidMethod(_jni_scanner_obj, _jni_scanner_release_table);
        _env->CallVoidMethod(_jni_scanner_obj, _jni_scanner_close);
        _env->DeleteLocalRef(_jni_scanner_obj);
        _env->DeleteLocalRef(_jni_scanner_cls);
        _closed = true;
        jthrowable exc = (_env)->ExceptionOccurred();
        if (exc != nullptr) {
            LOG(FATAL) << "Failed to release jni resource: "
                       << JniUtil::GetJniExceptionMsg(_env).to_string();
        }
    }
    return Status::OK();
}

Status JniConnector::_init_jni_scanner(JNIEnv* env, int batch_size) {
    RETURN_IF_ERROR(JniUtil::GetGlobalClassRef(env, _connector_class.c_str(), &_jni_scanner_cls));
    jmethodID scanner_constructor =
            env->GetMethodID(_jni_scanner_cls, "<init>", "(ILjava/util/Map;)V");
    RETURN_ERROR_IF_EXC(env);

    // prepare constructor parameters
    jclass hashmap_class = env->FindClass("java/util/HashMap");
    jmethodID hashmap_constructor = env->GetMethodID(hashmap_class, "<init>", "(I)V");
    jobject hashmap_object =
            env->NewObject(hashmap_class, hashmap_constructor, _scanner_params.size());
    jmethodID hashmap_put = env->GetMethodID(
            hashmap_class, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    RETURN_ERROR_IF_EXC(env);
    for (const auto& it : _scanner_params) {
        jstring key = env->NewStringUTF(it.first.c_str());
        jstring value = env->NewStringUTF(it.second.c_str());
        env->CallObjectMethod(hashmap_object, hashmap_put, key, value);
        env->DeleteLocalRef(key);
        env->DeleteLocalRef(value);
    }
    env->DeleteLocalRef(hashmap_class);
    _jni_scanner_obj =
            env->NewObject(_jni_scanner_cls, scanner_constructor, batch_size, hashmap_object);
    env->DeleteLocalRef(hashmap_object);
    RETURN_ERROR_IF_EXC(env);

    _jni_scanner_open = env->GetMethodID(_jni_scanner_cls, "open", "()V");
    RETURN_ERROR_IF_EXC(env);
    _jni_scanner_get_next_batch = env->GetMethodID(_jni_scanner_cls, "getNextBatchMeta", "()J");
    RETURN_ERROR_IF_EXC(env);
    _jni_scanner_close = env->GetMethodID(_jni_scanner_cls, "close", "()V");
    RETURN_ERROR_IF_EXC(env);
    _jni_scanner_release_column = env->GetMethodID(_jni_scanner_cls, "releaseColumn", "(I)V");
    RETURN_ERROR_IF_EXC(env);
    _jni_scanner_release_table = env->GetMethodID(_jni_scanner_cls, "releaseTable", "()V");
    RETURN_ERROR_IF_EXC(env);

    return Status::OK();
}

Status JniConnector::_fill_block(Block* block, size_t num_rows) {
    for (int i = 0; i < _column_names.size(); ++i) {
        auto& column_with_type_and_name = block->get_by_name(_column_names[i]);
        auto& column_ptr = column_with_type_and_name.column;
        auto& column_type = column_with_type_and_name.type;
        RETURN_IF_ERROR(_fill_column(column_ptr, column_type, num_rows));
        // Column is not released when _fill_column failed. It will be released when releasing table.
        _env->CallVoidMethod(_jni_scanner_obj, _jni_scanner_release_column, i);
        RETURN_ERROR_IF_EXC(_env);
    }
    return Status::OK();
}

Status JniConnector::_fill_column(ColumnPtr& doris_column, DataTypePtr& data_type,
                                  size_t num_rows) {
    TypeIndex logical_type = remove_nullable(data_type)->get_type_id();
    void* null_map_ptr = _next_meta_as_ptr();
    if (null_map_ptr == nullptr) {
        // org.apache.doris.jni.vec.ColumnType.Type#UNSUPPORTED will set column address as 0
        return Status::InternalError("Unsupported type {} in java side", getTypeName(logical_type));
    }
    MutableColumnPtr data_column;
    if (doris_column->is_nullable()) {
        auto* nullable_column = reinterpret_cast<vectorized::ColumnNullable*>(
                (*std::move(doris_column)).mutate().get());
        data_column = nullable_column->get_nested_column_ptr();
        NullMap& null_map = nullable_column->get_null_map_data();
        size_t origin_size = null_map.size();
        null_map.resize(origin_size + num_rows);
        memcpy(null_map.data() + origin_size, static_cast<bool*>(null_map_ptr), num_rows);
    } else {
        data_column = doris_column->assume_mutable();
    }
    // Date and DateTime are deprecated and not supported.
    switch (logical_type) {
#define DISPATCH(NUMERIC_TYPE, CPP_NUMERIC_TYPE)       \
    case NUMERIC_TYPE:                                 \
        return _fill_numeric_column<CPP_NUMERIC_TYPE>( \
                data_column, reinterpret_cast<CPP_NUMERIC_TYPE*>(_next_meta_as_ptr()), num_rows);
        FOR_LOGICAL_NUMERIC_TYPES(DISPATCH)
#undef DISPATCH
    case TypeIndex::Decimal128:
        [[fallthrough]];
    case TypeIndex::Decimal128I:
        return _fill_decimal_column<Int128>(
                data_column, reinterpret_cast<Int128*>(_next_meta_as_ptr()), num_rows);
    case TypeIndex::Decimal32:
        return _fill_decimal_column<Int32>(data_column,
                                           reinterpret_cast<Int32*>(_next_meta_as_ptr()), num_rows);
    case TypeIndex::Decimal64:
        return _fill_decimal_column<Int64>(data_column,
                                           reinterpret_cast<Int64*>(_next_meta_as_ptr()), num_rows);
    case TypeIndex::DateV2:
        return _decode_time_column<UInt32>(
                data_column, reinterpret_cast<UInt32*>(_next_meta_as_ptr()), num_rows);
    case TypeIndex::DateTimeV2:
        return _decode_time_column<UInt64>(
                data_column, reinterpret_cast<UInt64*>(_next_meta_as_ptr()), num_rows);
    case TypeIndex::String:
        [[fallthrough]];
    case TypeIndex::FixedString:
        return _fill_string_column(data_column, num_rows);
    default:
        return Status::InvalidArgument("Unsupported type {} in jni scanner",
                                       getTypeName(logical_type));
    }
    return Status::OK();
}

Status JniConnector::_fill_string_column(MutableColumnPtr& doris_column, size_t num_rows) {
    int* offsets = reinterpret_cast<int*>(_next_meta_as_ptr());
    char* data = reinterpret_cast<char*>(_next_meta_as_ptr());
    std::vector<StringRef> string_values;
    string_values.reserve(num_rows);
    for (size_t i = 0; i < num_rows; ++i) {
        int start_offset = i == 0 ? 0 : offsets[i - 1];
        int end_offset = offsets[i];
        string_values.emplace_back(data + start_offset, end_offset - start_offset);
    }
    doris_column->insert_many_strings(&string_values[0], num_rows);
    return Status::OK();
}

void JniConnector::_generate_predicates(
        std::unordered_map<std::string, ColumnValueRangeType>* colname_to_value_range) {
    if (colname_to_value_range == nullptr) {
        return;
    }
    for (auto& kv : *colname_to_value_range) {
        const std::string& column_name = kv.first;
        const ColumnValueRangeType& col_val_range = kv.second;
        std::visit([&](auto&& range) { _parse_value_range(range, column_name); }, col_val_range);
    }
}

std::string JniConnector::get_hive_type(const TypeDescriptor& desc) {
    std::ostringstream buffer;
    switch (desc.type) {
    case TYPE_BOOLEAN:
        return "boolean";
    case TYPE_TINYINT:
        return "tinyint";
    case TYPE_SMALLINT:
        return "smallint";
    case TYPE_INT:
        return "int";
    case TYPE_BIGINT:
        return "bigint";
    case TYPE_FLOAT:
        return "float";
    case TYPE_DOUBLE:
        return "double";
    case TYPE_VARCHAR: {
        buffer << "varchar(" << desc.len << ")";
        return buffer.str();
    }
    case TYPE_DATE:
        [[fallthrough]];
    case TYPE_DATEV2:
        return "date";
    case TYPE_DATETIME:
        [[fallthrough]];
    case TYPE_DATETIMEV2:
        [[fallthrough]];
    case TYPE_TIME:
        [[fallthrough]];
    case TYPE_TIMEV2:
        return "timestamp";
    case TYPE_BINARY:
        return "binary";
    case TYPE_CHAR: {
        buffer << "char(" << desc.len << ")";
        return buffer.str();
    }
    case TYPE_STRING:
        return "string";
    case TYPE_DECIMALV2: {
        buffer << "decimalv2(" << DecimalV2Value::PRECISION << "," << DecimalV2Value::SCALE << ")";
        return buffer.str();
    }
    case TYPE_DECIMAL32: {
        buffer << "decimal32(" << desc.precision << "," << desc.scale << ")";
        return buffer.str();
    }
    case TYPE_DECIMAL64: {
        buffer << "decimal64(" << desc.precision << "," << desc.scale << ")";
        return buffer.str();
    }
    case TYPE_DECIMAL128I: {
        buffer << "decimal128(" << desc.precision << "," << desc.scale << ")";
        return buffer.str();
    }
    case TYPE_STRUCT: {
        buffer << "struct<";
        for (int i = 0; i < desc.children.size(); ++i) {
            if (i != 0) {
                buffer << ",";
            }
            buffer << desc.field_names[i] << ":" << get_hive_type(desc.children[i]);
        }
        buffer << ">";
        return buffer.str();
    }
    case TYPE_ARRAY: {
        buffer << "array<" << get_hive_type(desc.children[0]) << ">";
        return buffer.str();
    }
    case TYPE_MAP: {
        buffer << "map<" << get_hive_type(desc.children[0]) << ","
               << get_hive_type(desc.children[1]) << ">";
        return buffer.str();
    }
    default:
        return "unsupported";
    }
}
} // namespace doris::vectorized