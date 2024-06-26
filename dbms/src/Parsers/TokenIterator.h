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

#include <Parsers/Lexer.h>

#include <vector>


namespace DB
{

/** Parser operates on lazy stream of tokens.
  * It could do lookaheads of any depth.
  */

/** Used as an input for parsers.
  * All whitespace and comment tokens are transparently skipped.
  */
class Tokens
{
private:
    std::vector<Token> data;
    Lexer lexer;

public:
    Tokens(const char * begin, const char * end, size_t max_query_size = 0)
        : lexer(begin, end, max_query_size)
    {}

    const Token & operator[](size_t index)
    {
        while (true)
        {
            if (index < data.size())
                return data[index];

            if (!data.empty() && data.back().isEnd())
                return data.back();

            Token token = lexer.nextToken();

            if (token.isSignificant())
                data.emplace_back(token);
        }
    }

    const Token & max()
    {
        if (data.empty())
            return (*this)[0];
        return data.back();
    }
};


/// To represent position in a token stream.
class TokenIterator
{
private:
    Tokens * tokens;
    size_t index = 0;

public:
    explicit TokenIterator(Tokens & tokens)
        : tokens(&tokens)
    {}

    const Token & get() { return (*tokens)[index]; }
    const Token & operator*() { return get(); }
    const Token * operator->() { return &get(); }

    TokenIterator & operator++()
    {
        ++index;
        return *this;
    }
    TokenIterator & operator--()
    {
        --index;
        return *this;
    }

    bool operator<(const TokenIterator & rhs) const { return index < rhs.index; }
    bool operator<=(const TokenIterator & rhs) const { return index <= rhs.index; }
    bool operator==(const TokenIterator & rhs) const { return index == rhs.index; }
    bool operator!=(const TokenIterator & rhs) const { return index != rhs.index; }

    bool isValid() { return get().type < TokenType::EndOfStream; }

    /// Rightmost token we had looked.
    const Token & max() { return tokens->max(); }
};


/// Returns positions of unmatched parentheses.
using UnmatchedParentheses = std::vector<Token>;
UnmatchedParentheses checkUnmatchedParentheses(TokenIterator begin, Token * last);

} // namespace DB
