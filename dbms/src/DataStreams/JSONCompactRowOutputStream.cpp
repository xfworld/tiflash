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

#include <DataStreams/JSONCompactRowOutputStream.h>
#include <IO/WriteHelpers.h>


namespace DB
{

JSONCompactRowOutputStream::JSONCompactRowOutputStream(
    WriteBuffer & ostr_,
    const Block & sample_,
    bool write_statistics_,
    const FormatSettingsJSON & settings_)
    : JSONRowOutputStream(ostr_, sample_, write_statistics_, settings_)
{}


void JSONCompactRowOutputStream::writeField(const IColumn & column, const IDataType & type, size_t row_num)
{
    type.serializeTextJSON(column, row_num, *ostr, settings);
    ++field_number;
}


void JSONCompactRowOutputStream::writeFieldDelimiter()
{
    writeCString(", ", *ostr);
}


void JSONCompactRowOutputStream::writeRowStartDelimiter()
{
    if (row_count > 0)
        writeCString(",\n", *ostr);
    writeCString("\t\t[", *ostr);
}


void JSONCompactRowOutputStream::writeRowEndDelimiter()
{
    writeChar(']', *ostr);
    field_number = 0;
    ++row_count;
}

static void writeExtremesElement(
    const char * title,
    const Block & extremes,
    size_t row_num,
    WriteBuffer & ostr,
    const FormatSettingsJSON & settings)
{
    writeCString("\t\t\"", ostr);
    writeCString(title, ostr);
    writeCString("\": [", ostr);

    size_t extremes_columns = extremes.columns();
    for (size_t i = 0; i < extremes_columns; ++i)
    {
        if (i != 0)
            writeChar(',', ostr);

        const ColumnWithTypeAndName & column = extremes.safeGetByPosition(i);
        column.type->serializeTextJSON(*column.column.get(), row_num, ostr, settings);
    }

    writeChar(']', ostr);
}

void JSONCompactRowOutputStream::writeExtremes()
{
    if (extremes)
    {
        writeCString(",\n", *ostr);
        writeChar('\n', *ostr);
        writeCString("\t\"extremes\":\n", *ostr);
        writeCString("\t{\n", *ostr);

        writeExtremesElement("min", extremes, 0, *ostr, settings);
        writeCString(",\n", *ostr);
        writeExtremesElement("max", extremes, 1, *ostr, settings);

        writeChar('\n', *ostr);
        writeCString("\t}", *ostr);
    }
}


} // namespace DB
