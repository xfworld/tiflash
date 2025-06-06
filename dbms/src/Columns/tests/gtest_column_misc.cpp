// Copyright 2024 PingCAP, Inc.
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

#include <Functions/FunctionHelpers.h>
#include <TestUtils/FunctionTestUtils.h>
#include <TestUtils/TiFlashTestBasic.h>
#include <common/types.h>

namespace DB
{
namespace tests
{
class TestColumnMisc : public ::testing::Test
{
public:
    static void testCloneFullColumn(const ColumnPtr & column_ptr)
    {
        auto col_ptr_str = column_ptr->dumpStructure();
        ColumnPtr col_ptr_clone = column_ptr->cloneFullColumn();
        ASSERT_COLUMN_EQ(column_ptr, col_ptr_clone);
        Field x;
        col_ptr_clone->get(0, x);
        col_ptr_clone->assumeMutable()->insert(x);
        // Test whether the clone is the deep copy, i.e the original column does not changed
        ASSERT_EQ(col_ptr_str, column_ptr->dumpStructure());

        auto col_nullmap = ColumnUInt8::create();
        for (size_t i = 0; i < col_ptr_clone->size(); ++i)
            col_nullmap->insert(FIELD_INT8_1);
        ColumnPtr col_nullable = ColumnNullable::create(col_ptr_clone, std::move(col_nullmap));
        auto col_nullable_str = col_nullable->dumpStructure();
        ColumnPtr col_nullable_clone = col_nullable->cloneFullColumn();
        ASSERT_COLUMN_EQ(col_nullable, col_nullable_clone);
        col_nullable_clone->get(0, x);
        col_nullable_clone->assumeMutable()->insert(x);
        // Test whether the clone is the deep copy, i.e the original column does not changed
        ASSERT_EQ(col_nullable_str, col_nullable->dumpStructure());
    }
};

TEST_F(TestColumnMisc, TestCloneFullColumn)
try
{
    auto col_vector = createColumn<UInt32>({1, 2, 3}).column;
    testCloneFullColumn(col_vector);
    auto col_decimal = createColumn<Decimal128>(std::make_tuple(10, 3), {"1234567.333"}).column;
    testCloneFullColumn(col_decimal);
    auto col_string = createColumn<String>({"sdafyuwer123"}).column;
    testCloneFullColumn(col_string);
    auto col_array = createColumn<Array>(
                         std::make_tuple(std::make_shared<DataTypeFloat32>()),
                         {Array{}, Array{1.0, 2.0}, Array{1.0, 2.0, 3.0}})
                         .column;
    testCloneFullColumn(col_array);
    ColumnPtr col_fixed_string = ColumnFixedString::create(2);
    col_fixed_string->assumeMutable()->insertData("12", 2);
    testCloneFullColumn(col_fixed_string);
}
CATCH

TEST_F(TestColumnMisc, TestSerializeByteSize)
try
{
    auto col_vector = createColumn<UInt32>({1, 2, 3}).column;
    ASSERT_EQ(col_vector->serializeByteSize(), sizeof(UInt32) * 3);
    auto col_decimal = createColumn<Decimal128>(std::make_tuple(10, 3), {"1234567.333", "23333.99"}).column;
    ASSERT_EQ(col_decimal->serializeByteSize(), sizeof(Decimal128) * 2);
    auto col_string = createColumn<String>({"abc", "def", "g", "hij", "", "mn"}).column;
    ASSERT_EQ(col_string->serializeByteSize(), sizeof(UInt32) * 6 + 18);

    auto nullable_col_vector = toNullableVec<UInt64>({1, 2, 3, 4, {}}).column;
    ASSERT_EQ(nullable_col_vector->serializeByteSize(), sizeof(UInt8) * 5 + sizeof(UInt64) * 5);
    auto nullable_col_decimal = createNullableColumn<Decimal256>(
                                    std::make_tuple(65, 30),
                                    {
                                        "123456789012345678901234567890",
                                        "100.1111111111",
                                        "-11111111111111111111",
                                        "0.1111111111111",
                                        "0.1111111111111",
                                        "2.2222222222",
                                    },
                                    {1, 0, 1, 1, 0, 1})
                                    .column;
    ASSERT_EQ(nullable_col_decimal->serializeByteSize(), sizeof(UInt8) * 6 + sizeof(Decimal256) * 6);
    auto nullable_col_string = toNullableVec<String>({"123456789", {}, "1"}).column;
    ASSERT_EQ(nullable_col_string->serializeByteSize(), sizeof(UInt8) * 3 + sizeof(UInt32) * 3 + 13);

    auto col_array = createColumn<Array>(
                         std::make_tuple(std::make_shared<DataTypeFloat32>()),
                         {Array{}, Array{1.0, 2.0}, Array{1.0, 2.0, 3.0}})
                         .column;
    ASSERT_EQ(col_array->serializeByteSize(), sizeof(UInt32) * 3 + sizeof(Float32) * 5);

    ColumnPtr col_fixed_string = ColumnFixedString::create(3);
    col_fixed_string->assumeMutable()->insertData("123", 2);
    col_fixed_string->assumeMutable()->insertData("12", 2);
    col_fixed_string->assumeMutable()->insertData("1", 1);
    ASSERT_EQ(col_fixed_string->serializeByteSize(), 3 * 3);
}
CATCH

} // namespace tests
} // namespace DB
