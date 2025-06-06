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

#pragma once

#include <AggregateFunctions/IAggregateFunction.h>
#include <Columns/ColumnDecimal.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnVector.h>
#include <Common/typeid_cast.h>
#include <DataTypes/IDataType.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <common/StringRef.h>

namespace DB
{
/** Aggregate functions that store one of passed values.
  * For example: min, max, any, anyLast.
  */

struct CommonImpl
{
    static void decrease(const IColumn &, size_t) { throw Exception("decrease is not implemented yet"); }
};

/// For numeric values.
template <typename T>
struct SingleValueDataFixed : public CommonImpl
{
protected:
    using Self = SingleValueDataFixed<T>;

    bool has_value
        = false; /// We need to remember if at least one value has been passed. This is necessary for AggregateFunctionIf.
    T value;

    using ColumnType = std::conditional_t<IsDecimal<T>, ColumnDecimal<T>, ColumnVector<T>>;

public:
    static bool needArena() { return false; }

    bool has() const { return has_value; }

    void setCollators(const TiDB::TiDBCollators &) {}

    void insertResultInto(IColumn & to) const
    {
        if (has())
            static_cast<ColumnType &>(to).getData().push_back(value);
        else
            static_cast<ColumnType &>(to).insertDefault();
    }

    void batchInsertSameResultInto(IColumn & to, size_t num) const
    {
        if (has())
        {
            auto & container = static_cast<ColumnType &>(to).getData();
            container.resize_fill(num + container.size(), value);
        }
        else
        {
            static_cast<ColumnType &>(to).insertManyDefaults(num);
        }
    }


    void write(WriteBuffer & buf, const IDataType & /*data_type*/) const
    {
        writeBinary(has(), buf);
        if (has())
            writeBinary(value, buf);
    }

    void read(ReadBuffer & buf, const IDataType & /*data_type*/, Arena *)
    {
        readBinary(has_value, buf);
        if (has())
            readBinary(value, buf);
    }

    void change(const IColumn & column, size_t row_num, Arena *)
    {
        has_value = true;
        value = static_cast<const ColumnType &>(column).getData()[row_num];
    }

    /// Assuming to.has()
    void change(const Self & to, Arena *)
    {
        has_value = true;
        value = to.value;
    }

    bool changeFirstTime(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has())
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeFirstTime(const Self & to, Arena * arena)
    {
        if (!has() && to.has())
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeEveryTime(const IColumn & column, size_t row_num, Arena * arena)
    {
        change(column, row_num, arena);
        return true;
    }

    bool changeEveryTime(const Self & to, Arena * arena)
    {
        if (to.has())
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfLess(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has() || static_cast<const ColumnType &>(column).getData()[row_num] < value)
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfLess(const Self & to, Arena * arena)
    {
        if (to.has() && (!has() || to.value < value))
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfGreater(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has() || static_cast<const ColumnType &>(column).getData()[row_num] > value)
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfGreater(const Self & to, Arena * arena)
    {
        if (to.has() && (!has() || to.value > value))
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool isEqualTo(const Self & to) const { return has() && to.value == value; }

    bool isEqualTo(const IColumn & column, size_t row_num) const
    {
        return has() && static_cast<const ColumnType &>(column).getData()[row_num] == value;
    }

    void reset() { has_value = false; }
};


/** For strings. Short strings are stored in the object itself, and long strings are allocated separately.
  * NOTE It could also be suitable for arrays of numbers.
  */
struct SingleValueDataString : public CommonImpl
{
protected:
    using Self = SingleValueDataString;

    Int32 size = -1; /// -1 indicates that there is no value.
    Int32 capacity = 0; /// power of two or zero
    char * large_data{};
    TiDB::TiDBCollatorPtr collator{};

    bool less(const StringRef & a, const StringRef & b) const
    {
        if (unlikely(collator == nullptr))
            return a < b;
        return collator->compareFastPath(a.data, a.size, b.data, b.size) < 0;
    }

    bool greater(const StringRef & a, const StringRef & b) const
    {
        if (unlikely(collator == nullptr))
            return a > b;
        return collator->compareFastPath(a.data, a.size, b.data, b.size) > 0;
    }

    bool equalTo(const StringRef & a, const StringRef & b) const
    {
        if (unlikely(collator == nullptr))
            return a == b;
        return collator->compareFastPath(a.data, a.size, b.data, b.size) == 0;
    }

public:
    static constexpr Int32 AUTOMATIC_STORAGE_SIZE = 64;
    static constexpr Int32 MAX_SMALL_STRING_SIZE
        = AUTOMATIC_STORAGE_SIZE - sizeof(size) - sizeof(capacity) - sizeof(large_data) - sizeof(TiDB::TiDBCollatorPtr);

protected:
    char small_data[MAX_SMALL_STRING_SIZE]{}; /// Including the terminating zero.

public:
    static bool needArena() { return true; }

    bool has() const { return size >= 0; }

    const char * getData() const { return size <= MAX_SMALL_STRING_SIZE ? small_data : large_data; }

    StringRef getStringRef() const { return StringRef(getData(), size); }

    void insertResultInto(IColumn & to) const
    {
        if (has())
            static_cast<ColumnString &>(to).insertDataWithTerminatingZero(getData(), size);
        else
            static_cast<ColumnString &>(to).insertDefault();
    }

    void batchInsertSameResultInto(IColumn & to, size_t num) const
    {
        if (has())
            static_cast<ColumnString &>(to).batchInsertDataWithTerminatingZero(num, getData(), size);
        else
            static_cast<ColumnString &>(to).insertManyDefaults(num);
    }

    void setCollators(const TiDB::TiDBCollators & collators_)
    {
        collator = !collators_.empty() ? collators_[0] : nullptr;
    }

    void write(WriteBuffer & buf, const IDataType & /*data_type*/) const
    {
        writeBinary(size, buf);
        writeBinary(collator == nullptr ? 0 : collator->getCollatorId(), buf);
        if (has())
            buf.write(getData(), size);
    }

    void read(ReadBuffer & buf, const IDataType & /*data_type*/, Arena * arena)
    {
        Int32 rhs_size;
        readBinary(rhs_size, buf);
        Int32 collator_id;
        readBinary(collator_id, buf);
        if (collator_id != 0)
            collator = TiDB::ITiDBCollator::getCollator(collator_id);
        else
            collator = nullptr;

        if (rhs_size >= 0)
        {
            if (rhs_size <= MAX_SMALL_STRING_SIZE)
            {
                /// Don't free large_data here.

                size = rhs_size;

                if (size > 0)
                    buf.read(small_data, size);
            }
            else
            {
                if (capacity < rhs_size)
                {
                    capacity = static_cast<UInt32>(roundUpToPowerOfTwoOrZero(rhs_size));
                    /// Don't free large_data here.
                    large_data = arena->alloc(capacity);
                }

                size = rhs_size;
                buf.read(large_data, size);
            }
        }
        else
        {
            /// Don't free large_data here.
            size = rhs_size;
        }
    }

    /// Assuming to.has()
    void changeImpl(StringRef value, Arena * arena)
    {
        Int32 value_size = value.size;

        if (value_size <= MAX_SMALL_STRING_SIZE)
        {
            /// Don't free large_data here.
            size = value_size;

            if (size > 0)
                memcpy(small_data, value.data, size);
        }
        else
        {
            if (capacity < value_size)
            {
                /// Don't free large_data here.
                capacity = roundUpToPowerOfTwoOrZero(value_size);
                large_data = arena->alloc(capacity);
            }

            size = value_size;
            memcpy(large_data, value.data, size);
        }
    }

    void change(const IColumn & column, size_t row_num, Arena * arena)
    {
        changeImpl(static_cast<const ColumnString &>(column).getDataAtWithTerminatingZero(row_num), arena);
    }

    void change(const Self & to, Arena * arena) { changeImpl(to.getStringRef(), arena); }

    bool changeFirstTime(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has())
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeFirstTime(const Self & to, Arena * arena)
    {
        if (!has() && to.has())
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeEveryTime(const IColumn & column, size_t row_num, Arena * arena)
    {
        change(column, row_num, arena);
        return true;
    }

    bool changeEveryTime(const Self & to, Arena * arena)
    {
        if (to.has())
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfLess(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has()
            || less(static_cast<const ColumnString &>(column).getDataAtWithTerminatingZero(row_num), getStringRef()))
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfLess(const Self & to, Arena * arena)
    {
        // todo should check the collator in `to` and `this`
        if (to.has() && (!has() || less(to.getStringRef(), getStringRef())))
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfGreater(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has()
            || greater(static_cast<const ColumnString &>(column).getDataAtWithTerminatingZero(row_num), getStringRef()))
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfGreater(const Self & to, Arena * arena)
    {
        if (to.has() && (!has() || greater(to.getStringRef(), getStringRef())))
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool isEqualTo(const Self & to) const { return has() && equalTo(to.getStringRef(), getStringRef()); }

    bool isEqualTo(const IColumn & column, size_t row_num) const
    {
        return has()
            && equalTo(static_cast<const ColumnString &>(column).getDataAtWithTerminatingZero(row_num), getStringRef());
    }

    void reset() { size = -1; }
};

static_assert(
    sizeof(SingleValueDataString) == SingleValueDataString::AUTOMATIC_STORAGE_SIZE,
    "Incorrect size of SingleValueDataString struct");


/// For any other value types.
struct SingleValueDataGeneric : public CommonImpl
{
protected:
    using Self = SingleValueDataGeneric;

    Field value;

public:
    static bool needArena() { return false; }

    bool has() const { return !value.isNull(); }

    void setCollators(const TiDB::TiDBCollators &) {}

    void insertResultInto(IColumn & to) const
    {
        if (has())
            to.insert(value);
        else
            to.insertDefault();
    }

    void batchInsertSameResultInto(IColumn & to, size_t num) const
    {
        if (has())
        {
            to.insertMany(value, num);
        }
        else
            to.insertManyDefaults(num);
    }

    void write(WriteBuffer & buf, const IDataType & data_type) const
    {
        if (!value.isNull())
        {
            writeBinary(true, buf);
            data_type.serializeBinary(value, buf);
        }
        else
            writeBinary(false, buf);
    }

    void read(ReadBuffer & buf, const IDataType & data_type, Arena *)
    {
        bool is_not_null;
        readBinary(is_not_null, buf);

        if (is_not_null)
            data_type.deserializeBinary(value, buf);
    }

    void change(const IColumn & column, size_t row_num, Arena *) { column.get(row_num, value); }

    void change(const Self & to, Arena *) { value = to.value; }

    bool changeFirstTime(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has())
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeFirstTime(const Self & to, Arena * arena)
    {
        if (!has() && to.has())
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeEveryTime(const IColumn & column, size_t row_num, Arena * arena)
    {
        change(column, row_num, arena);
        return true;
    }

    bool changeEveryTime(const Self & to, Arena * arena)
    {
        if (to.has())
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfLess(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has())
        {
            change(column, row_num, arena);
            return true;
        }
        else
        {
            Field new_value;
            column.get(row_num, new_value);

            if (new_value < value)
            {
                value = new_value;
                return true;
            }
            else
                return false;
        }
    }

    bool changeIfLess(const Self & to, Arena * arena)
    {
        if (to.has() && (!has() || to.value < value))
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfGreater(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has())
        {
            change(column, row_num, arena);
            return true;
        }
        else
        {
            Field new_value;
            column.get(row_num, new_value);

            if (new_value > value)
            {
                value = new_value;
                return true;
            }
            else
                return false;
        }
    }

    bool changeIfGreater(const Self & to, Arena * arena)
    {
        if (to.has() && (!has() || to.value > value))
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool isEqualTo(const IColumn & column, size_t row_num) const { return has() && value == column[row_num]; }

    bool isEqualTo(const Self & to) const { return has() && to.value == value; }

    void reset() { value = Field(); }
};


/** What is the difference between the aggregate functions min, max, any, anyLast
  *  (the condition that the stored value is replaced by a new one,
  *   as well as, of course, the name).
  */

template <typename Data>
struct AggregateFunctionMinData : Data
{
    using Self = AggregateFunctionMinData<Data>;

    bool changeIfBetter(const IColumn & column, size_t row_num, Arena * arena)
    {
        return this->changeIfLess(column, row_num, arena);
    }
    bool changeIfBetter(const Self & to, Arena * arena) { return this->changeIfLess(to, arena); }

    static const char * name() { return "min"; }
};

template <typename Data>
struct AggregateFunctionMaxData : Data
{
    using Self = AggregateFunctionMaxData<Data>;

    bool changeIfBetter(const IColumn & column, size_t row_num, Arena * arena)
    {
        return this->changeIfGreater(column, row_num, arena);
    }

    bool changeIfBetter(const Self & to, Arena * arena) { return this->changeIfGreater(to, arena); }

    static const char * name() { return "max"; }
};

template <typename Data>
struct AggregateFunctionAnyData : Data
{
    using Self = AggregateFunctionAnyData<Data>;

    bool changeIfBetter(const IColumn & column, size_t row_num, Arena * arena)
    {
        return this->changeFirstTime(column, row_num, arena);
    }
    bool changeIfBetter(const Self & to, Arena * arena) { return this->changeFirstTime(to, arena); }

    static const char * name() { return "any"; }
};

template <typename Data>
struct AggregateFunctionFirstRowData : Data
{
    using Self = AggregateFunctionFirstRowData<Data>;

    bool changeIfBetter(const IColumn & column, size_t row_num, Arena * arena)
    {
        return this->changeFirstTime(column, row_num, arena);
    }
    bool changeIfBetter(const Self & to, Arena * arena) { return this->changeFirstTime(to, arena); }

    static const char * name() { return "first_row"; }
};

template <typename Data>
struct AggregateFunctionAnyLastData : Data
{
    using Self = AggregateFunctionAnyLastData<Data>;

    bool changeIfBetter(const IColumn & column, size_t row_num, Arena * arena)
    {
        return this->changeEveryTime(column, row_num, arena);
    }
    bool changeIfBetter(const Self & to, Arena * arena) { return this->changeEveryTime(to, arena); }

    static const char * name() { return "anyLast"; }
};


/** Implement 'heavy hitters' algorithm.
  * Selects most frequent value if its frequency is more than 50% in each thread of execution.
  * Otherwise, selects some arbitary value.
  * http://www.cs.umd.edu/~samir/498/karp.pdf
  */
template <typename Data>
struct AggregateFunctionAnyHeavyData : Data
{
    size_t counter = 0;

    using Self = AggregateFunctionAnyHeavyData<Data>;

    bool changeIfBetter(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (this->isEqualTo(column, row_num))
        {
            ++counter;
        }
        else
        {
            if (counter == 0)
            {
                this->change(column, row_num, arena);
                ++counter;
                return true;
            }
            else
                --counter;
        }
        return false;
    }

    bool changeIfBetter(const Self & to, Arena * arena)
    {
        if (this->isEqualTo(to))
        {
            counter += to.counter;
        }
        else
        {
            if (counter < to.counter)
            {
                this->change(to, arena);
                return true;
            }
            else
                counter -= to.counter;
        }
        return false;
    }

    void write(WriteBuffer & buf, const IDataType & data_type) const
    {
        Data::write(buf, data_type);
        writeBinary(counter, buf);
    }

    void read(ReadBuffer & buf, const IDataType & data_type, Arena * arena)
    {
        Data::read(buf, data_type, arena);
        readBinary(counter, buf);
    }

    static const char * name() { return "anyHeavy"; }
};


template <typename Data>
class AggregateFunctionsSingleValue final
    : public IAggregateFunctionDataHelper<Data, AggregateFunctionsSingleValue<Data>, true>
{
private:
    DataTypePtr type;

public:
    explicit AggregateFunctionsSingleValue(const DataTypePtr & type)
        : type(type)
    {
        if (StringRef(Data::name()) == StringRef("min") || StringRef(Data::name()) == StringRef("max")
            || StringRef(Data::name()) == StringRef("max_for_window")
            || StringRef(Data::name()) == StringRef("min_for_window"))
        {
            if (!type->isComparable())
                throw Exception(
                    "Illegal type " + type->getName() + " of argument of aggregate function " + getName()
                        + " because the values of that data type are not comparable",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        }
    }

    String getName() const override { return Data::name(); }

    DataTypePtr getReturnType() const override { return type; }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena * arena) const override
    {
        this->data(place).changeIfBetter(*columns[0], row_num, arena);
    }

    void decrease(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        this->data(place).decrease(*columns[0], row_num);
    }

    void reset(AggregateDataPtr __restrict place) const override { this->data(place).reset(); }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena * arena) const override
    {
        this->data(place).changeIfBetter(this->data(rhs), arena);
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf) const override
    {
        this->data(place).write(buf, *type.get());
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, Arena * arena) const override
    {
        this->data(place).read(buf, *type.get(), arena);
    }

    void insertResultInto(ConstAggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        this->data(place).insertResultInto(to);
    }

    void batchInsertSameResultInto(ConstAggregateDataPtr __restrict place, IColumn & to, size_t num) const override
    {
        this->data(place).batchInsertSameResultInto(to, num);
    }

    const char * getHeaderFilePath() const override { return __FILE__; }

    bool allocatesMemoryInArena() const override { return Data::needArena(); }
};

} // namespace DB
