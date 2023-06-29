// Copyright (C) 2023 The JLLVM Contributors.
//
// This file is part of JLLVM.
//
// JLLVM is free software; you can redistribute it and/or modify it under  the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 3, or (at your option) any later version.
//
// JLLVM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with JLLVM; see the file LICENSE.txt.  If not
// see <http://www.gnu.org/licenses/>.

#include "Descriptors.hpp"

#include <llvm/Support/ErrorHandling.h>

#include <cassert>

namespace
{
jllvm::FieldType parseFieldTypeImpl(llvm::StringRef& string)
{
    auto first = string.front();
    string = string.drop_front();
    switch (first)
    {
        case 'B': return jllvm::BaseType::Byte;
        case 'C': return jllvm::BaseType::Char;
        case 'D': return jllvm::BaseType::Double;
        case 'F': return jllvm::BaseType::Float;
        case 'I': return jllvm::BaseType::Int;
        case 'J': return jllvm::BaseType::Long;
        case 'S': return jllvm::BaseType::Short;
        case 'Z': return jllvm::BaseType::Boolean;
        case 'V': return jllvm::BaseType::Void;
        case '[': return jllvm::ArrayType{std::make_unique<jllvm::FieldType>(parseFieldTypeImpl(string))};
        case 'L':
        {
            auto clazz = string.take_while([](char c) { return c != ';'; });
            string = string.drop_front(clazz.size() + 1);
            return jllvm::ObjectType{clazz};
        }
        default: llvm::report_fatal_error("Invalid descriptor");
    }
}
} // namespace

jllvm::FieldType jllvm::parseFieldType(llvm::StringRef string)
{
    return parseFieldTypeImpl(string);
}

jllvm::MethodType jllvm::parseMethodType(llvm::StringRef string)
{
    assert(string.front() == '(');
    string = string.drop_front();
    std::vector<FieldType> parameters;
    while (string.front() != ')')
    {
        parameters.push_back(parseFieldTypeImpl(string));
    }
    string = string.drop_front();
    return MethodType{std::move(parameters), parseFieldTypeImpl(string)};
}
