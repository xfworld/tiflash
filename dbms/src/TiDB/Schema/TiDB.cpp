// Copyright 2023 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Common/Decimal.h>
#include <Common/Exception.h>
#include <Common/MyTime.h>
#include <Core/Types.h>
#include <DataTypes/DataTypeDecimal.h>
#include <DataTypes/FieldToDataType.h>
#include <IO/Buffer/ReadBufferFromString.h>
#include <Poco/Base64Decoder.h>
#include <Poco/Dynamic/Var.h>
#include <Poco/MemoryStream.h>
#include <Poco/StreamCopier.h>
#include <Poco/StringTokenizer.h>
#include <Storages/MutableSupport.h>
#include <TiDB/Collation/Collator.h>
#include <TiDB/Decode/DatumCodec.h>
#include <TiDB/Decode/JsonBinary.h>
#include <TiDB/Decode/Vector.h>
#include <TiDB/Schema/FullTextIndex.h>
#include <TiDB/Schema/SchemaNameMapper.h>
#include <TiDB/Schema/TiDB.h>
#include <TiDB/Schema/VectorIndex.h>
#include <clara_fts/src/tokenizer/mod.rs.h>
#include <common/logger_useful.h>
#include <fmt/format.h>
#include <tipb/executor.pb.h>

#include <algorithm>
#include <cmath>
#include <magic_enum.hpp>
#include <string>

namespace DB
{
namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
extern const int INCORRECT_DATA;
} // namespace ErrorCodes
extern const UInt8 TYPE_CODE_LITERAL;
extern const UInt8 LITERAL_NIL;

Field GenDefaultField(const TiDB::ColumnInfo & col_info)
{
    switch (col_info.getCodecFlag())
    {
    case TiDB::CodecFlagNil:
        return Field();
    case TiDB::CodecFlagBytes:
        return Field(String());
    case TiDB::CodecFlagDecimal:
    {
        auto type = createDecimal(col_info.flen, col_info.decimal);
        if (checkDecimal<Decimal32>(*type))
            return Field(DecimalField<Decimal32>(Decimal32(), col_info.decimal));
        else if (checkDecimal<Decimal64>(*type))
            return Field(DecimalField<Decimal64>(Decimal64(), col_info.decimal));
        else if (checkDecimal<Decimal128>(*type))
            return Field(DecimalField<Decimal128>(Decimal128(), col_info.decimal));
        else
            return Field(DecimalField<Decimal256>(Decimal256(), col_info.decimal));
    }
    break;
    case TiDB::CodecFlagCompactBytes:
        return Field(String());
    case TiDB::CodecFlagFloat:
        return Field(static_cast<Float64>(0));
    case TiDB::CodecFlagUInt:
        return Field(static_cast<UInt64>(0));
    case TiDB::CodecFlagInt:
        return Field(static_cast<Int64>(0));
    case TiDB::CodecFlagVarInt:
        return Field(static_cast<Int64>(0));
    case TiDB::CodecFlagVarUInt:
        return Field(static_cast<UInt64>(0));
    case TiDB::CodecFlagJson:
        return TiDB::genJsonNull();
    case TiDB::CodecFlagVectorFloat32:
        return Field(Array(0));
    case TiDB::CodecFlagDuration:
        return Field(static_cast<Int64>(0));
    default:
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Not implemented codec flag: {}",
            fmt::underlying(col_info.getCodecFlag()));
    }
}
} // namespace DB

namespace TiDB
{
using DB::Decimal128;
using DB::Decimal256;
using DB::Decimal32;
using DB::Decimal64;
using DB::DecimalField;
using DB::Exception;
using DB::Field;
using DB::SchemaNameMapper;

// The IndexType defined in TiDB
// https://github.com/pingcap/tidb/blob/84492a9a1e5bff0b4a4256955ab8231975c2dde1/pkg/parser/ast/model.go#L217-L226
enum class IndexType
{
    INVALID = 0,
    BTREE = 1,
    HASH = 2,
    RTREE = 3,
    HYPO = 4,
    VECTOR = 5,
    INVERTED = 6,
    // Note: HNSW here only for complementary purpose.
    // It shall never be used, because TiDB only use it as a parser token and will
    // never leak it to the outside.
    // HNSW = 7,
};

FullTextIndexDefinitionPtr parseFullTextIndexFromJSON(const Poco::JSON::Object::Ptr & json)
{
    RUNTIME_CHECK(json); // not nullptr

    RUNTIME_CHECK_MSG(json->has("parser_type"), "Invalid FullTextIndex definition, missing parser_type");
    auto parser_type_field = json->getValue<String>("parser_type");
    RUNTIME_CHECK_MSG(
        ClaraFTS::supports_tokenizer(parser_type_field),
        "Invalid FullTextIndex definition, unsupported parser_type `{}`",
        parser_type_field);

    return std::make_shared<const FullTextIndexDefinition>(FullTextIndexDefinition{
        .parser_type = parser_type_field,
    });
}

Poco::JSON::Object::Ptr fullTextIndexToJSON(const FullTextIndexDefinitionPtr & full_text_index)
{
    RUNTIME_CHECK(full_text_index != nullptr);
    RUNTIME_CHECK(ClaraFTS::supports_tokenizer(full_text_index->parser_type));

    Poco::JSON::Object::Ptr json = new Poco::JSON::Object();
    json->set("parser_type", full_text_index->parser_type);
    return json;
}

VectorIndexDefinitionPtr parseVectorIndexFromJSON(const Poco::JSON::Object::Ptr & json)
{
    assert(json); // not nullptr

    auto dimension = json->getValue<UInt64>("dimension");
    RUNTIME_CHECK(dimension > 0 && dimension <= TiDB::MAX_VECTOR_DIMENSION, dimension); // Just a protection

    tipb::VectorDistanceMetric distance_metric = tipb::VectorDistanceMetric::INVALID_DISTANCE_METRIC;
    auto distance_metric_field = json->getValue<String>("distance_metric");
    RUNTIME_CHECK_MSG(
        tipb::VectorDistanceMetric_Parse(distance_metric_field, &distance_metric),
        "invalid distance_metric of vector index, {}",
        distance_metric_field);
    RUNTIME_CHECK(distance_metric != tipb::VectorDistanceMetric::INVALID_DISTANCE_METRIC);

    return std::make_shared<const VectorIndexDefinition>(VectorIndexDefinition{
        // TODO: To be removed. We will not expose real algorithm in future.
        .kind = tipb::VectorIndexKind::HNSW,
        .dimension = dimension,
        .distance_metric = distance_metric,
    });
}

Poco::JSON::Object::Ptr vectorIndexToJSON(const VectorIndexDefinitionPtr & vector_index)
{
    assert(vector_index != nullptr);
    RUNTIME_CHECK(vector_index->kind != tipb::VectorIndexKind::INVALID_INDEX_KIND);
    RUNTIME_CHECK(vector_index->distance_metric != tipb::VectorDistanceMetric::INVALID_DISTANCE_METRIC);

    Poco::JSON::Object::Ptr vector_index_json = new Poco::JSON::Object();
    vector_index_json->set("kind", tipb::VectorIndexKind_Name(vector_index->kind));
    vector_index_json->set("dimension", vector_index->dimension);
    vector_index_json->set("distance_metric", tipb::VectorDistanceMetric_Name(vector_index->distance_metric));
    return vector_index_json;
}

InvertedIndexDefinitionPtr parseInvertedIndexFromJSON(IndexType index_type, const Poco::JSON::Object::Ptr & json)
{
    assert(json); // not nullptr

    RUNTIME_CHECK(index_type == IndexType::INVERTED);
    bool is_signed = json->getValue<bool>("is_signed");
    auto type_size = json->getValue<UInt8>("type_size");
    RUNTIME_CHECK(type_size > 0 && type_size <= sizeof(UInt64), type_size); // Just a protection
    return std::make_shared<const InvertedIndexDefinition>(InvertedIndexDefinition{
        .is_signed = is_signed,
        .type_size = type_size,
    });
}

Poco::JSON::Object::Ptr invertedIndexToJSON(const InvertedIndexDefinitionPtr & inverted_index)
{
    assert(inverted_index != nullptr);
    RUNTIME_CHECK(inverted_index->type_size > 0 && inverted_index->type_size <= sizeof(UInt64));

    Poco::JSON::Object::Ptr inverted_index_json = new Poco::JSON::Object();
    inverted_index_json->set("is_signed", inverted_index->is_signed);
    inverted_index_json->set("type_size", inverted_index->type_size);
    return inverted_index_json;
}

////////////////////////
////// ColumnInfo //////
////////////////////////

ColumnInfo::ColumnInfo(Poco::JSON::Object::Ptr json)
{
    deserialize(json);
}

#define TRY_CATCH_DEFAULT_VALUE_TO_FIELD(try_block) \
    try                                             \
    {                                               \
        try_block                                   \
    }                                               \
    catch (...)                                     \
    {                                               \
        return DB::GenDefaultField(*this);          \
    }


Field ColumnInfo::defaultValueToField() const
{
    const auto & value = origin_default_value;
    const auto & bit_value = origin_default_bit_value;
    if (value.isEmpty() && bit_value.isEmpty())
    {
        if (hasNotNullFlag())
            return DB::GenDefaultField(*this);
        return Field();
    }
    switch (tp)
    {
    // Integer Type.
    // In c++, cast a unsigned integer to signed integer will not change the value.
    // like 9223372036854775808 which is larger than the maximum value of Int64,
    // static_cast<UInt64>(static_cast<Int64>(9223372036854775808)) == 9223372036854775808
    // so we don't need consider unsigned here.
    case TypeTiny:
    case TypeShort:
    case TypeLong:
    case TypeLongLong:
    case TypeInt24:
        TRY_CATCH_DEFAULT_VALUE_TO_FIELD({
            try
            {
                return value.convert<Int64>();
            }
            catch (...)
            {
                // due to https://github.com/pingcap/tidb/issues/34881
                // we do this to avoid exception in older version of TiDB.
                return static_cast<Int64>(std::llround(value.convert<double>()));
            }
        });
    case TypeBit:
    {
        TRY_CATCH_DEFAULT_VALUE_TO_FIELD({
            // When we got bit_value from tipb, we have decoded it.
            if (auto is_int = bit_value.isInteger(); is_int)
                return bit_value.convert<UInt64>();
            return getBitValue(bit_value.convert<String>());
        });
    }
    // Floating type.
    case TypeFloat:
    case TypeDouble:
        TRY_CATCH_DEFAULT_VALUE_TO_FIELD({ return value.convert<double>(); });
    case TypeDate:
    case TypeDatetime:
    case TypeTimestamp:
        TRY_CATCH_DEFAULT_VALUE_TO_FIELD({
            // When we got value from tipb, we have decoded it.
            if (auto is_int = value.isInteger(); is_int)
                return value.convert<UInt64>();
            return DB::parseMyDateTime(value.convert<String>());
        });
    case TypeVarchar:
    case TypeTinyBlob:
    case TypeMediumBlob:
    case TypeLongBlob:
    case TypeBlob:
    case TypeVarString:
    case TypeString:
    {
        auto v = value.convert<String>();
        if (hasBinaryFlag())
        {
            // For some binary column(like varchar(20)), we have to pad trailing zeros according to the specified type length.
            // User may define default value `0x1234` for a `BINARY(4)` column, TiDB stores it in a string "\u12\u34" (sized 2).
            // But it actually means `0x12340000`.
            // And for some binary column(like longblob), we do not need to pad trailing zeros.
            // And the `Flen` is set to -1, therefore we need to check `Flen >= 0` here.
            if (Int32 vlen = v.length(); flen >= 0 && vlen < flen)
                v.append(flen - vlen, '\0');
        }
        return v;
    }
    case TypeJSON:
        // JSON can't have a default value
        return genJsonNull();
    case TypeEnum:
        TRY_CATCH_DEFAULT_VALUE_TO_FIELD({ return getEnumIndex(value.convert<String>()); });
    case TypeNull:
        return Field();
    case TypeDecimal:
    case TypeNewDecimal:
        TRY_CATCH_DEFAULT_VALUE_TO_FIELD({
            auto text = value.convert<String>();
            if (text.empty())
                return DB::GenDefaultField(*this);
            return getDecimalValue(text);
        });
    case TypeTime:
        TRY_CATCH_DEFAULT_VALUE_TO_FIELD({
            // When we got value from tipb, we have decoded it.
            if (auto is_int = value.isInteger(); is_int)
                return value.convert<UInt64>();
            return getTimeValue(value.convert<String>());
        });
    case TypeYear:
        // Never throw exception here, do not use TRY_CATCH_DEFAULT_VALUE_TO_FIELD
        return getYearValue(value.convert<String>());
    case TypeSet:
        TRY_CATCH_DEFAULT_VALUE_TO_FIELD({
            // When we got value from tipb, we have decoded it.
            if (auto is_int = value.isInteger(); is_int)
                return value.convert<UInt64>();
            return getSetValue(value.convert<String>());
        });
    case TypeTiDBVectorFloat32:
        return genVectorFloat32Empty();
    default:
        throw Exception("Have not processed type: " + std::to_string(tp));
    }
    return Field();
}

#undef TRY_CATCH_DEFAULT_VALUE_TO_FIELD

DB::Field ColumnInfo::getDecimalValue(const String & decimal_text) const
{
    DB::ReadBufferFromString buffer(decimal_text);
    auto precision = flen;
    auto scale = decimal;

    auto type = DB::createDecimal(precision, scale);
    if (DB::checkDecimal<Decimal32>(*type))
    {
        DB::Decimal32 result;
        DB::readDecimalText(result, buffer, precision, scale);
        return DecimalField<Decimal32>(result, scale);
    }
    else if (DB::checkDecimal<Decimal64>(*type))
    {
        DB::Decimal64 result;
        DB::readDecimalText(result, buffer, precision, scale);
        return DecimalField<Decimal64>(result, scale);
    }
    else if (DB::checkDecimal<Decimal128>(*type))
    {
        DB::Decimal128 result;
        DB::readDecimalText(result, buffer, precision, scale);
        return DecimalField<Decimal128>(result, scale);
    }
    else
    {
        DB::Decimal256 result;
        DB::readDecimalText(result, buffer, precision, scale);
        return DecimalField<Decimal256>(result, scale);
    }
}

// FIXME it still has bug: https://github.com/pingcap/tidb/issues/11435
Int64 ColumnInfo::getEnumIndex(const String & enum_id_or_text) const
{
    const auto * collator = ITiDBCollator::getCollator(collate.isEmpty() ? "binary" : collate.convert<String>());
    if (!collator)
        // TODO: if new collation is enabled, should use "utf8mb4_bin"
        collator = ITiDBCollator::getCollator("binary");
    for (const auto & elem : elems)
    {
        if (collator->compareFastPath(
                elem.first.data(),
                elem.first.size(),
                enum_id_or_text.data(),
                enum_id_or_text.size()) //
            == 0)
        {
            return elem.second;
        }
    }
    return std::stoi(enum_id_or_text);
}

UInt64 ColumnInfo::getSetValue(const String & set_str) const
{
    const auto * collator = ITiDBCollator::getCollator(collate.isEmpty() ? "binary" : collate.convert<String>());
    if (!collator)
        // TODO: if new collation is enabled, should use "utf8mb4_bin"
        collator = ITiDBCollator::getCollator("binary");
    std::string sort_key_container;
    Poco::StringTokenizer string_tokens(set_str, ",");
    std::set<String> marked;
    for (const auto & s : string_tokens)
        marked.insert(collator->sortKeyFastPath(s.data(), s.length(), sort_key_container).toString());

    UInt64 value = 0;
    for (size_t i = 0; i < elems.size(); i++)
    {
        String key = collator->sortKeyFastPath(elems.at(i).first.data(), elems.at(i).first.length(), sort_key_container)
                         .toString();
        auto it = marked.find(key);
        if (it != marked.end())
        {
            value |= 1ULL << i;
            marked.erase(it);
        }
    }

    if (marked.empty())
        return value;

    return 0;
}

Int64 ColumnInfo::getTimeValue(const String & time_str)
{
    const static int64_t fractional_seconds_multiplier[]
        = {1000000000, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};
    bool negative = time_str[0] == '-';
    Poco::StringTokenizer second_and_fsp(time_str, ".");
    Poco::StringTokenizer string_tokens(second_and_fsp[0], ":");
    Int64 ret = 0;
    for (auto const & s : string_tokens)
        ret = ret * 60 + std::abs(std::stoi(s));
    Int32 fs_length = 0;
    Int64 fs_value = 0;
    if (second_and_fsp.count() == 2)
    {
        fs_length = second_and_fsp[1].length();
        fs_value = std::stol(second_and_fsp[1]);
    }
    ret = ret * fractional_seconds_multiplier[0] + fs_value * fractional_seconds_multiplier[fs_length];
    return negative ? -ret : ret;
}

Int64 ColumnInfo::getYearValue(const String & val)
{
    // make sure the year is non-negative integer
    if (val.empty() || !std::all_of(val.begin(), val.end(), ::isdigit))
        return 0;
    Int64 year = std::stol(val);
    if (0 < year && year < 70)
        return 2000 + year;
    if (70 <= year && year < 100)
        return 1900 + year;
    if (year == 0 && val.length() <= 2)
        return 2000;
    return year;
}

UInt64 ColumnInfo::getBitValue(const String & val)
{
    // The `default_bit` is a base64 encoded, big endian byte array.
    Poco::MemoryInputStream istr(val.data(), val.size());
    Poco::Base64Decoder decoder(istr);
    std::string decoded;
    Poco::StreamCopier::copyToString(decoder, decoded);
    UInt64 result = 0;
    for (auto c : decoded)
    {
        result = result << 8 | c;
    }
    return result;
}

Poco::JSON::Object::Ptr ColumnInfo::getJSONObject() const
try
{
    Poco::JSON::Object::Ptr json = new Poco::JSON::Object();

    json->set("id", id);
    Poco::JSON::Object::Ptr name_json = new Poco::JSON::Object();
    name_json->set("O", name);
    name_json->set("L", name);
    json->set("name", name_json);
    json->set("offset", offset);
    if (!origin_default_value.isEmpty())
        json->set("origin_default", origin_default_value);
    if (!default_value.isEmpty())
        json->set("default", default_value);
    if (!default_bit_value.isEmpty())
        json->set("default_bit", default_bit_value);
    if (!origin_default_bit_value.isEmpty())
        json->set("origin_default_bit", origin_default_bit_value);
    {
        // "type" field
        Poco::JSON::Object::Ptr tp_json = new Poco::JSON::Object();
        tp_json->set("Tp", static_cast<Int32>(tp));
        tp_json->set("Flag", flag);
        tp_json->set("Flen", flen);
        tp_json->set("Decimal", decimal);
        if (!charset.isEmpty())
            tp_json->set("Charset", charset);
        if (!collate.isEmpty())
            tp_json->set("Collate", collate);
        if (!elems.empty())
        {
            Poco::JSON::Array::Ptr elem_arr = new Poco::JSON::Array();
            for (const auto & elem : elems)
                elem_arr->add(elem.first);
            tp_json->set("Elems", elem_arr);
        }
        json->set("type", tp_json);
    }
    json->set("state", static_cast<Int32>(state));

#ifndef NDEBUG
    // Check stringify in Debug mode
    std::stringstream str;
    json->stringify(str);
#endif

    return json;
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Serialize TiDB schema JSON failed (ColumnInfo): " + e.displayText(),
        DB::Exception(e));
}

void ColumnInfo::deserialize(Poco::JSON::Object::Ptr json)
try
{
    id = json->getValue<ColumnID>("id");
    name = json->getObject("name")->getValue<String>("L");
    offset = json->getValue<Int32>("offset");
    if (!json->isNull("origin_default"))
        origin_default_value = json->get("origin_default");
    if (!json->isNull("default"))
        default_value = json->get("default");
    if (!json->isNull("default_bit"))
        default_bit_value = json->get("default_bit");
    if (!json->isNull("origin_default_bit"))
        origin_default_bit_value = json->get("origin_default_bit");
    {
        // type
        auto type_json = json->getObject("type");
        tp = static_cast<TP>(type_json->getValue<Int32>("Tp"));
        flag = type_json->getValue<UInt32>("Flag");
        flen = type_json->getValue<Int64>("Flen");
        decimal = type_json->getValue<Int64>("Decimal");
        if (!type_json->isNull("Elems"))
        {
            auto elems_arr = type_json->getArray("Elems");
            size_t elems_size = elems_arr->size();
            for (size_t i = 1; i <= elems_size; i++)
            {
                elems.push_back(std::make_pair(elems_arr->getElement<String>(i - 1), static_cast<Int16>(i)));
            }
        }
        /// need to do this check for forward compatibility
        if (!type_json->isNull("Charset"))
            charset = type_json->get("Charset");
        /// need to do this check for forward compatibility
        if (!type_json->isNull("Collate"))
            collate = type_json->get("Collate");
    }
    state = static_cast<SchemaState>(json->getValue<Int32>("state"));
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Parse TiDB schema JSON failed (ColumnInfo): " + e.displayText(),
        DB::Exception(e));
}

///////////////////////////
////// PartitionInfo //////
///////////////////////////

PartitionDefinition::PartitionDefinition(Poco::JSON::Object::Ptr json)
{
    deserialize(json);
}

Poco::JSON::Object::Ptr PartitionDefinition::getJSONObject() const
try
{
    Poco::JSON::Object::Ptr json = new Poco::JSON::Object();
    json->set("id", id);
    Poco::JSON::Object::Ptr name_json = new Poco::JSON::Object();
    name_json->set("O", name);
    name_json->set("L", name);
    json->set("name", name_json);

#ifndef NDEBUG
    // Check stringify in Debug mode
    std::stringstream str;
    json->stringify(str);
#endif

    return json;
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Serialize TiDB schema JSON failed (PartitionDef): " + e.displayText(),
        DB::Exception(e));
}

void PartitionDefinition::deserialize(Poco::JSON::Object::Ptr json)
try
{
    id = json->getValue<TableID>("id");
    name = json->getObject("name")->getValue<String>("L");
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Parse TiDB schema JSON failed (PartitionDefinition): " + e.displayText(),
        DB::Exception(e));
}

PartitionInfo::PartitionInfo(Poco::JSON::Object::Ptr json)
{
    deserialize(json);
}

Poco::JSON::Object::Ptr PartitionInfo::getJSONObject() const
try
{
    Poco::JSON::Object::Ptr json = new Poco::JSON::Object();

    json->set("type", static_cast<Int32>(type));
    json->set("expr", expr);
    json->set("enable", enable);
    json->set("num", num);

    Poco::JSON::Array::Ptr def_arr = new Poco::JSON::Array();

    for (const auto & part_def : definitions)
    {
        def_arr->add(part_def.getJSONObject());
    }

    json->set("definitions", def_arr);

#ifndef NDEBUG
    // Check stringify in Debug mode
    std::stringstream str;
    json->stringify(str);
#endif

    return json;
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Serialize TiDB schema JSON failed (PartitionInfo): " + e.displayText(),
        DB::Exception(e));
}

void PartitionInfo::deserialize(Poco::JSON::Object::Ptr json)
try
{
    type = static_cast<PartitionType>(json->getValue<Int32>("type"));
    expr = json->getValue<String>("expr");
    enable = json->getValue<bool>("enable");

    auto defs_json = json->getArray("definitions");
    definitions.clear();
    std::unordered_set<TableID> part_id_set;
    for (size_t i = 0; i < defs_json->size(); i++)
    {
        PartitionDefinition definition(defs_json->getObject(i));
        definitions.emplace_back(definition);
        part_id_set.emplace(definition.id);
    }

    /// Treat `adding_definitions` and `dropping_definitions` as the normal `definitions`
    /// in TiFlash. Because TiFlash need to create the physical IStorage instance
    /// to handle the data on those partitions during DDL.

    auto add_defs_json = json->getArray("adding_definitions");
    if (!add_defs_json.isNull())
    {
        for (size_t i = 0; i < add_defs_json->size(); i++)
        {
            PartitionDefinition definition(add_defs_json->getObject(i));
            if (part_id_set.count(definition.id) == 0)
            {
                definitions.emplace_back(definition);
                part_id_set.emplace(definition.id);
            }
        }
    }

    auto drop_defs_json = json->getArray("dropping_definitions");
    if (!drop_defs_json.isNull())
    {
        for (size_t i = 0; i < drop_defs_json->size(); i++)
        {
            PartitionDefinition definition(drop_defs_json->getObject(i));
            if (part_id_set.count(definition.id) == 0)
            {
                definitions.emplace_back(definition);
                part_id_set.emplace(definition.id);
            }
        }
    }

    num = json->getValue<UInt64>("num");
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Parse TiDB schema JSON failed (PartitionInfo): " + e.displayText(),
        DB::Exception(e));
}

////////////////////////////////
////// TiFlashReplicaInfo //////
////////////////////////////////

Poco::JSON::Object::Ptr TiFlashReplicaInfo::getJSONObject() const
try
{
    Poco::JSON::Object::Ptr json = new Poco::JSON::Object();
    json->set("Count", count);
    if (available)
    {
        json->set("Available", *available);
    }

#ifndef NDEBUG
    // Check stringify in Debug mode
    std::stringstream str;
    json->stringify(str);
#endif

    return json;
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__)
            + ": Serialize TiDB schema JSON failed (TiFlashReplicaInfo): " + e.displayText(),
        DB::Exception(e));
}

void TiFlashReplicaInfo::deserialize(Poco::JSON::Object::Ptr & json)
try
{
    count = json->getValue<UInt64>("Count");
    if (json->has("Available"))
    {
        available = json->getValue<bool>("Available");
    }
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        String(__PRETTY_FUNCTION__) + ": Parse TiDB schema JSON failed (TiFlashReplicaInfo): " + e.displayText(),
        DB::Exception(e));
}

////////////////////
////// DBInfo //////
////////////////////

String DBInfo::serialize() const
try
{
    std::stringstream buf;

    Poco::JSON::Object::Ptr json = new Poco::JSON::Object();
    json->set("id", id);
    json->set("keyspace_id", keyspace_id);
    Poco::JSON::Object::Ptr name_json = new Poco::JSON::Object();
    name_json->set("O", name);
    name_json->set("L", name);
    json->set("db_name", name_json);

    json->set("charset", charset);
    json->set("collate", collate);

    json->set("state", static_cast<Int32>(state));

    json->stringify(buf);

    return buf.str();
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Serialize TiDB schema JSON failed (DBInfo): " + e.displayText(),
        DB::Exception(e));
}

void DBInfo::deserialize(const String & json_str)
try
{
    Poco::JSON::Parser parser;
    Poco::Dynamic::Var result = parser.parse(json_str);
    auto obj = result.extract<Poco::JSON::Object::Ptr>();
    id = obj->getValue<DatabaseID>("id");
    if (obj->has("keyspace_id"))
    {
        keyspace_id = obj->getValue<KeyspaceID>("keyspace_id");
    }
    name = obj->get("db_name").extract<Poco::JSON::Object::Ptr>()->get("L").convert<String>();
    charset = obj->get("charset").convert<String>();
    collate = obj->get("collate").convert<String>();
    state = static_cast<SchemaState>(obj->getValue<Int32>("state"));
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Parse TiDB schema JSON failed (DBInfo): " + e.displayText()
            + ", json: " + json_str,
        DB::Exception(e));
}

///////////////////////
/// IndexColumnInfo ///
///////////////////////

IndexColumnInfo::IndexColumnInfo(Poco::JSON::Object::Ptr json)
    : length(0)
    , offset(0)
{
    deserialize(json);
}

Poco::JSON::Object::Ptr IndexColumnInfo::getJSONObject() const
try
{
    Poco::JSON::Object::Ptr json = new Poco::JSON::Object();

    Poco::JSON::Object::Ptr name_json = new Poco::JSON::Object();
    name_json->set("O", name);
    name_json->set("L", name);
    json->set("name", name_json);
    json->set("offset", offset);
    json->set("length", length);

#ifndef NDEBUG
    std::stringstream str;
    json->stringify(str);
#endif

    return json;
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Serialize TiDB schema JSON failed (IndexColumnInfo): " + e.displayText(),
        DB::Exception(e));
}

void IndexColumnInfo::deserialize(Poco::JSON::Object::Ptr json)
try
{
    name = json->getObject("name")->getValue<String>("L");
    offset = json->getValue<Int32>("offset");
    length = json->getValue<Int32>("length");
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Parse TiDB schema JSON failed (IndexColumnInfo): " + e.displayText(),
        DB::Exception(e));
}

///////////////////////
////// IndexInfo //////
///////////////////////

IndexInfo::IndexInfo(Poco::JSON::Object::Ptr json)
{
    deserialize(json);
}

Poco::JSON::Object::Ptr IndexInfo::getJSONObject() const
try
{
    Poco::JSON::Object::Ptr json = new Poco::JSON::Object();

    json->set("id", id);

    Poco::JSON::Object::Ptr idx_name_json = new Poco::JSON::Object();
    idx_name_json->set("O", idx_name);
    idx_name_json->set("L", idx_name);
    json->set("idx_name", idx_name_json);

    Poco::JSON::Array::Ptr cols_array = new Poco::JSON::Array();
    for (const auto & col : idx_cols)
    {
        auto col_obj = col.getJSONObject();
        cols_array->add(col_obj);
    }
    json->set("idx_cols", cols_array);
    json->set("state", static_cast<Int32>(state));
    json->set("index_type", index_type);
    json->set("is_unique", is_unique);
    json->set("is_primary", is_primary);
    json->set("is_invisible", is_invisible);
    json->set("is_global", is_global);

    if (vector_index)
    {
        json->set("vector_index", vectorIndexToJSON(vector_index));
    }
    else if (inverted_index)
    {
        json->set("inverted_index", invertedIndexToJSON(inverted_index));
    }
    else if (full_text_index)
    {
        json->set("full_text_index", fullTextIndexToJSON(full_text_index));
    }

#ifndef NDEBUG
    std::stringstream str;
    json->stringify(str);
#endif

    return json;
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Serialize TiDB schema JSON failed (IndexInfo): " + e.displayText(),
        DB::Exception(e));
}

void IndexInfo::deserialize(Poco::JSON::Object::Ptr json)
try
{
    id = json->getValue<Int64>("id");
    idx_name = json->getObject("idx_name")->getValue<String>("L");

    auto cols_array = json->getArray("idx_cols");
    idx_cols.clear();
    if (!cols_array.isNull())
    {
        for (size_t i = 0; i < cols_array->size(); i++)
        {
            auto col_json = cols_array->getObject(i);
            IndexColumnInfo column_info(col_json);
            idx_cols.emplace_back(column_info);
        }
    }

    state = static_cast<SchemaState>(json->getValue<Int32>("state"));
    index_type = json->getValue<Int32>("index_type");
    is_unique = json->getValue<bool>("is_unique");
    is_primary = json->getValue<bool>("is_primary");
    if (json->has("is_invisible"))
        is_invisible = json->getValue<bool>("is_invisible");
    if (json->has("is_global"))
        is_global = json->getValue<bool>("is_global");

    if (auto vector_index_json = json->getObject("vector_index"); vector_index_json)
    {
        RUNTIME_CHECK(static_cast<IndexType>(index_type) == IndexType::VECTOR);
        vector_index = parseVectorIndexFromJSON(vector_index_json);
    }
    if (auto inverted_index_json = json->getObject("inverted_index"); inverted_index_json)
    {
        inverted_index = parseInvertedIndexFromJSON(static_cast<IndexType>(index_type), inverted_index_json);
    }
    if (auto full_text_index_json = json->getObject("full_text_index"); full_text_index_json)
    {
        full_text_index = parseFullTextIndexFromJSON(full_text_index_json);
    }
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Deserialize TiDB schema JSON failed (IndexInfo): " + e.displayText(),
        DB::Exception(e));
}

///////////////////////
////// TableInfo //////
///////////////////////
TableInfo::TableInfo(Poco::JSON::Object::Ptr json, KeyspaceID keyspace_id_)
{
    deserialize(json);
    if (keyspace_id == NullspaceID)
    {
        keyspace_id = keyspace_id_;
    }
}

TableInfo::TableInfo(const String & table_info_json, KeyspaceID keyspace_id_)
{
    deserialize(table_info_json);
    // If the table_info_json has no keyspace id, we use the keyspace_id_ as the default value.
    if (keyspace_id == NullspaceID)
    {
        keyspace_id = keyspace_id_;
    }
}

String TableInfo::serialize() const
try
{
    Poco::JSON::Object::Ptr json = new Poco::JSON::Object();
    json->set("id", id);
    json->set("keyspace_id", keyspace_id);
    Poco::JSON::Object::Ptr name_json = new Poco::JSON::Object();
    name_json->set("O", name);
    name_json->set("L", name);
    json->set("name", name_json);

    Poco::JSON::Array::Ptr cols_arr = new Poco::JSON::Array();
    for (const auto & col_info : columns)
    {
        auto col_obj = col_info.getJSONObject();
        cols_arr->add(col_obj);
    }
    json->set("cols", cols_arr);

    Poco::JSON::Array::Ptr index_arr = new Poco::JSON::Array();
    for (const auto & index_info : index_infos)
    {
        auto index_info_obj = index_info.getJSONObject();
        index_arr->add(index_info_obj);
    }
    json->set("index_info", index_arr);
    json->set("state", static_cast<Int32>(state));
    json->set("pk_is_handle", pk_is_handle);
    json->set("is_common_handle", is_common_handle);
    json->set("update_timestamp", update_timestamp);
    if (is_partition_table)
    {
        json->set("belonging_table_id", belonging_table_id);
        if (belonging_table_id == DB::InvalidTableID)
        {
            // We use `belonging_table_id == DB::InvalidTableID` for the
            // logical partition table.
            // Only record partition info in LogicalPartitionTable
            json->set("partition", partition.getJSONObject());
        }
    }

    json->set("tiflash_replica", replica_info.getJSONObject());

    std::stringstream buf;
    json->stringify(buf);
    return buf.str();
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Serialize TiDB schema JSON failed (TableInfo): " + e.displayText(),
        DB::Exception(e));
}

String JSONToString(Poco::JSON::Object::Ptr json)
{
    std::stringstream buf;
    json->stringify(buf);
    return buf.str();
}

void TableInfo::deserialize(Poco::JSON::Object::Ptr obj)
try
{
    id = obj->getValue<TableID>("id");
    if (obj->has("keyspace_id"))
    {
        keyspace_id = obj->getValue<KeyspaceID>("keyspace_id");
    }
    name = obj->getObject("name")->getValue<String>("L");

    columns.clear();
    if (auto cols_arr = obj->getArray("cols"); !cols_arr.isNull())
    {
        for (size_t i = 0; i < cols_arr->size(); i++)
        {
            auto col_json = cols_arr->getObject(i);
            ColumnInfo column_info(col_json);
            columns.emplace_back(column_info);
        }
    }

    index_infos.clear();
    bool has_primary_index = false;
    if (auto index_arr = obj->getArray("index_info"); !index_arr.isNull())
    {
        for (size_t i = 0; i < index_arr->size(); i++)
        {
            auto index_info_json = index_arr->getObject(i);
            IndexInfo index_info(index_info_json);
            // We only keep the "primary index" or "vector index" in tiflash now
            if (index_info.is_primary)
            {
                has_primary_index = true;
                // always put the primary_index at the front of all index_info
                index_infos.insert(index_infos.begin(), std::move(index_info));
            }
            else if (index_info.isColumnarIndex())
            {
                index_infos.emplace_back(std::move(index_info));
            }
        }
    }

    state = static_cast<SchemaState>(obj->getValue<Int32>("state"));
    pk_is_handle = obj->getValue<bool>("pk_is_handle");
    if (obj->has("is_common_handle"))
        is_common_handle = obj->getValue<bool>("is_common_handle");
    if (obj->has("update_timestamp"))
        update_timestamp = obj->getValue<Timestamp>("update_timestamp");
    auto partition_obj = obj->getObject("partition");
    is_partition_table = obj->has("belonging_table_id") || !partition_obj.isNull();
    if (is_partition_table)
    {
        if (obj->has("belonging_table_id"))
            belonging_table_id = obj->getValue<TableID>("belonging_table_id");
        if (!partition_obj.isNull())
            partition.deserialize(partition_obj);
    }
    if (obj->has("view") && !obj->getObject("view").isNull())
    {
        is_view = true;
    }
    if (obj->has("sequence") && !obj->getObject("sequence").isNull())
    {
        is_sequence = true;
    }
    if (obj->has("tiflash_replica"))
    {
        if (auto replica_obj = obj->getObject("tiflash_replica"); !replica_obj.isNull())
        {
            replica_info.deserialize(replica_obj);
        }
    }
    if (is_common_handle && !has_primary_index)
    {
        throw DB::Exception(
            DB::ErrorCodes::INCORRECT_DATA,
            "{}: Parse TiDB schema JSON failed (TableInfo): clustered index without primary key info, json: ",
            __PRETTY_FUNCTION__,
            JSONToString(obj));
    }
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Parse TiDB schema JSON failed (TableInfo): " + e.displayText()
            + ", json: " + JSONToString(obj),
        DB::Exception(e));
}

void TableInfo::deserialize(const String & json_str)
try
{
    if (json_str.empty())
    {
        id = DB::InvalidTableID;
        return;
    }

    Poco::JSON::Parser parser;
    Poco::Dynamic::Var result = parser.parse(json_str);

    const auto & obj = result.extract<Poco::JSON::Object::Ptr>();
    deserialize(obj);
}
catch (const Poco::Exception & e)
{
    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Parse TiDB schema JSON failed (TableInfo): " + e.displayText()
            + ", json: " + json_str,
        DB::Exception(e));
}

template <CodecFlag cf>
CodecFlag getCodecFlagBase(bool /*unsigned_flag*/)
{
    return cf;
}

template <>
CodecFlag getCodecFlagBase<CodecFlagVarInt>(bool unsigned_flag)
{
    return unsigned_flag ? CodecFlagVarUInt : CodecFlagVarInt;
}

template <>
CodecFlag getCodecFlagBase<CodecFlagInt>(bool unsigned_flag)
{
    return unsigned_flag ? CodecFlagUInt : CodecFlagInt;
}

CodecFlag ColumnInfo::getCodecFlag() const
{
    switch (tp)
    {
#ifdef M
#error "Please undefine macro M first."
#endif
#define M(tt, v, cf, ct) \
    case Type##tt:       \
        return getCodecFlagBase<CodecFlag##cf>(hasUnsignedFlag());
        COLUMN_TYPES(M)
#undef M
    }

    throw Exception("Unknown CodecFlag", DB::ErrorCodes::LOGICAL_ERROR);
}

ColumnID TableInfo::getColumnID(const String & name) const
{
    for (const auto & col : columns)
    {
        if (name == col.name)
        {
            return col.id;
        }
    }

    if (name == DB::MutSup::extra_handle_column_name)
        return DB::MutSup::extra_handle_id;
    else if (name == DB::MutSup::version_column_name)
        return DB::MutSup::version_col_id;
    else if (name == DB::MutSup::delmark_column_name)
        return DB::MutSup::delmark_col_id;

    DB::Strings available_columns;
    for (const auto & c : columns)
    {
        available_columns.emplace_back(c.name);
    }

    throw DB::Exception(
        DB::ErrorCodes::LOGICAL_ERROR,
        "Fail to get column id from TableInfo, table_id={} name={} available_columns={}",
        id,
        name,
        available_columns);
}

KeyspaceID TableInfo::getKeyspaceID() const
{
    return keyspace_id;
}

const IndexInfo & TableInfo::getPrimaryIndexInfo() const
{
    assert(is_common_handle);
#ifndef NDEBUG
    RUNTIME_CHECK(index_infos[0].is_primary);
#endif
    return index_infos[0];
}

size_t TableInfo::numColumnsInKey() const
{
    if (pk_is_handle)
        return 1;
    else if (is_common_handle)
        return getPrimaryIndexInfo().idx_cols.size();
    return 0;
}

String TableInfo::getColumnName(const ColumnID id) const
{
    for (const auto & col : columns)
    {
        if (id == col.id)
        {
            return col.name;
        }
    }

    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Invalidate column id " + std::to_string(id) + " for table " + name,
        DB::ErrorCodes::LOGICAL_ERROR);
}

const ColumnInfo & TableInfo::getColumnInfo(const ColumnID id) const
{
    for (const auto & col : columns)
    {
        if (id == col.id)
        {
            return col;
        }
    }

    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Invalidate column id " + std::to_string(id) + " for table " + name,
        DB::ErrorCodes::LOGICAL_ERROR);
}

std::optional<std::reference_wrapper<const ColumnInfo>> TableInfo::getPKHandleColumn() const
{
    if (!pk_is_handle)
        return std::nullopt;

    for (const auto & col : columns)
    {
        if (col.hasPriKeyFlag())
            return std::optional<std::reference_wrapper<const ColumnInfo>>(col);
    }

    throw DB::Exception(
        std::string(__PRETTY_FUNCTION__) + ": Cannot get handle column for table " + name,
        DB::ErrorCodes::LOGICAL_ERROR);
}

TableInfoPtr TableInfo::producePartitionTableInfo(TableID table_or_partition_id, const SchemaNameMapper & name_mapper)
    const
{
    // Some sanity checks for partition table.
    if (unlikely(!(is_partition_table && partition.enable)))
        throw Exception(
            DB::ErrorCodes::LOGICAL_ERROR,
            "Try to produce partition table on a non-partition table, table_id={} partition_table_id={}",
            id,
            table_or_partition_id);

    if (unlikely(
            std::find_if(
                partition.definitions.begin(),
                partition.definitions.end(),
                [table_or_partition_id](const auto & d) { return d.id == table_or_partition_id; })
            == partition.definitions.end()))
    {
        std::vector<TableID> part_ids;
        std::for_each(
            partition.definitions.begin(),
            partition.definitions.end(),
            [&part_ids](const PartitionDefinition & part) { part_ids.emplace_back(part.id); });
        throw Exception(
            DB::ErrorCodes::LOGICAL_ERROR,
            "Can not find partition id in partition table, partition_table_id={} logical_table_id={} "
            "available_ids={}",
            table_or_partition_id,
            id,
            part_ids);
    }

    // This is a TiDB partition table, adjust the table ID by making it to physical table ID (partition ID).
    auto new_table = std::make_shared<TableInfo>();
    *new_table = *this;
    new_table->belonging_table_id = id;
    new_table->id = table_or_partition_id;

    new_table->name = name_mapper.mapPartitionName(*new_table);

    new_table->replica_info = replica_info;

    return new_table;
}

String genJsonNull()
{
    // null
    const static String null(
        {static_cast<char>(DB::JsonBinary::TYPE_CODE_LITERAL), static_cast<char>(DB::JsonBinary::LITERAL_NIL)});
    return null;
}

String genVectorFloat32Empty()
{
    return String(4, '\0'); // Length=0 vector
}

tipb::FieldType columnInfoToFieldType(const ColumnInfo & ci)
{
    tipb::FieldType ret;
    ret.set_tp(ci.tp);
    ret.set_flag(ci.flag);
    ret.set_flen(ci.flen);
    ret.set_decimal(ci.decimal);
    if (!ci.collate.isEmpty())
    {
        auto collator_name = ci.collate.convert<String>();
        TiDBCollatorPtr collator = ITiDBCollator::getCollator(collator_name);
        RUNTIME_CHECK_MSG(collator, "cannot find collator: {}", collator_name);
        ret.set_collate(collator->getCollatorId());
    }
    for (const auto & elem : ci.elems)
    {
        ret.add_elems(elem.first);
    }
    return ret;
}

ColumnInfo fieldTypeToColumnInfo(const tipb::FieldType & field_type)
{
    TiDB::ColumnInfo ret;
    ret.tp = static_cast<TiDB::TP>(field_type.tp());
    ret.flag = field_type.flag();
    ret.flen = field_type.flen();
    ret.decimal = field_type.decimal();
    for (int i = 0; i < field_type.elems_size(); i++)
    {
        ret.elems.emplace_back(field_type.elems(i), i + 1);
    }
    return ret;
}

ColumnInfo toTiDBColumnInfo(const tipb::ColumnInfo & tipb_column_info)
{
    ColumnInfo tidb_column_info;
    tidb_column_info.tp = static_cast<TiDB::TP>(tipb_column_info.tp());
    tidb_column_info.id = tipb_column_info.column_id();
    tidb_column_info.flag = tipb_column_info.flag();
    tidb_column_info.flen = tipb_column_info.columnlen();
    tidb_column_info.decimal = tipb_column_info.decimal();
    tidb_column_info.collate = tipb_column_info.collation();
    for (int i = 0; i < tipb_column_info.elems_size(); ++i)
        tidb_column_info.elems.emplace_back(tipb_column_info.elems(i), i + 1);
    // TiFlash get default value from origin_default_value, check `Field ColumnInfo::defaultValueToField() const`
    // So we need to set origin_default_value to tipb_column_info.default_val()
    // Related logic in tidb, https://github.com/pingcap/tidb/blob/45318da24d8e4c0c6aab836d291a33f949dd18bf/pkg/table/tables/tables.go#L2303-L2329
    // And, decode tipb_column_info.default_val.
    {
        // The default value is null.
        if (tipb_column_info.default_val().empty())
        {
            Poco::Dynamic::Var empty_val;
            tidb_column_info.origin_default_value = empty_val;
            return tidb_column_info;
        }
        size_t cursor = 0;
        auto val = DB::DecodeDatum(cursor, tipb_column_info.default_val());
        if (val.getType() == DB::Field::Types::String)
        {
            tidb_column_info.origin_default_value = val.get<String>();
            return tidb_column_info;
        }
        switch (tidb_column_info.tp)
        {
        case TypeDate:
        case TypeDatetime:
        case TypeTimestamp:
        case TypeSet:
            tidb_column_info.origin_default_value = val.get<UInt64>();
            break;
        case TypeTime:
            tidb_column_info.origin_default_value = val.get<Int64>();
            break;
        case TypeBit:
            // For TypeBit, we need to set origin_default_bit_value to tipb_column_info.default_val().
            tidb_column_info.origin_default_bit_value = val.get<UInt64>();
            break;
        case TypeYear:
            // If the year column has 'not null' option and the value is 0, this year value is '0000'.
            if (val.get<Int64>() == 0)
            {
                Poco::Dynamic::Var empty_val;
                tidb_column_info.origin_default_value = empty_val;
                break;
            }
            // else fallthrough
        // The above types will be processed again when defaultValueToField is called.
        // By distinguishing the type of the string type(by synchronizing the default values in the schema, its type is string),
        // we can distinguish whether it needs to be processed.
        default:
            auto str_val = DB::applyVisitor(DB::FieldVisitorToString(false), val);
            tidb_column_info.origin_default_value = str_val;
        }
    }

    return tidb_column_info;
}

std::vector<ColumnInfo> toTiDBColumnInfos(
    const ::google::protobuf::RepeatedPtrField<tipb::ColumnInfo> & tipb_column_infos)
{
    std::vector<ColumnInfo> tidb_column_infos;
    tidb_column_infos.reserve(tipb_column_infos.size());
    for (const auto & tipb_column_info : tipb_column_infos)
        tidb_column_infos.emplace_back(toTiDBColumnInfo(tipb_column_info));
    return tidb_column_infos;
}

} // namespace TiDB
