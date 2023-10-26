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

std::string jllvm::FieldType::textual() const
{
    std::string result(m_arrayCount, '[');
    if (m_name)
    {
        result += 'L';
        result.append(m_name, m_size);
        result += ';';
        return result;
    }

    switch (m_baseTypeValue)
    {
        case BaseType::Byte: return result + 'B';
        case BaseType::Char: return result + 'C';
        case BaseType::Double: return result + 'D';
        case BaseType::Float: return result + 'F';
        case BaseType::Int: return result + 'I';
        case BaseType::Long: return result + 'J';
        case BaseType::Short: return result + 'S';
        case BaseType::Void: return result + 'V';
        case BaseType::Boolean: return result + 'Z';
        default: llvm_unreachable("Invalid value");
    }
}

std::string jllvm::FieldType::pretty() const
{
    std::string result;
    if (m_name)
    {
        std::replace_copy_if(
            m_name, m_name + m_size, std::back_inserter(result), [](char c) { return c == '/'; }, '.');
    }
    else
    {
        llvm::StringRef baseType;
        switch (m_baseTypeValue)
        {
            case BaseType::Byte:
            {
                baseType = "byte";
                break;
            }
            case BaseType::Char:
            {
                baseType = "char";
                break;
            }
            case BaseType::Double:
            {
                baseType = "double";
                break;
            }
            case BaseType::Float:
            {
                baseType = "float";
                break;
            }
            case BaseType::Int:
            {
                baseType = "int";
                break;
            }
            case BaseType::Long:
            {
                baseType = "long";
                break;
            }
            case BaseType::Short:
            {
                baseType = "short";
                break;
            }
            case BaseType::Void:
            {
                baseType = "void";
                break;
            }
            case BaseType::Boolean:
            {
                baseType = "boolean";
                break;
            }
            default: llvm_unreachable("Invalid value");
        }
        result.append(baseType);
    }

    for (auto i = 0; i < m_arrayCount; i++)
    {
        result.append("[]");
    }

    return result;
}
