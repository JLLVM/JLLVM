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

#include "ClassFile.hpp"

#include <llvm/Support/ErrorHandling.h>

using namespace jllvm;

namespace
{

template <class T>
concept FieldOrMethodInfo = llvm::is_one_of<T, FieldInfo, MethodInfo>::value;

std::uint8_t deduceByteCount(std::uint8_t c)
{
    if (c <= 0x7F)
    {
        return 1;
    }
    if ((c & 0xE0) == 0b11000000)
    {
        return 2;
    }
    if ((c & 0xF0) == 0b11100000)
    {
        return 3;
    }
    llvm::report_fatal_error("Invalid JVM UTF-8 encoding");
}

std::string toUTF8(llvm::StringRef rawString)
{
    std::string result;
    result.reserve(rawString.size());
    for (const char* iter = rawString.begin(); iter != rawString.end();)
    {
        char firstByte = *iter;
        switch (deduceByteCount(firstByte))
        {
            case 1:
                iter++;
                result.push_back(firstByte);
                continue;
            case 2:
            {
                iter++;
                std::uint8_t secondByte = *iter++;
                std::uint16_t codepoint = ((firstByte & 0x1f) << 6) + (secondByte & 0x3f);
                if (codepoint == 0)
                {
                    result.push_back(0);
                    continue;
                }
                result.push_back(firstByte);
                result.push_back(secondByte);
                continue;
            }
            case 3:
            {
                std::uint8_t u = *iter++;
                std::uint8_t v = *iter++;
                std::uint8_t w = *iter++;
                if (iter == rawString.end() || deduceByteCount(*iter) != 3
                    || (u != 0b11101101 && static_cast<std::uint8_t>(*iter) != 0b11101101))
                {
                    result.push_back(u);
                    result.push_back(v);
                    result.push_back(w);
                    continue;
                }
                iter++;
                std::uint8_t y = *iter++;
                std::uint8_t z = *iter++;

                std::uint32_t codepoint =
                    0x10000 + ((v & 0x0f) << 16) + ((w & 0x3f) << 10) + ((y & 0x0f) << 6) + (z & 0x3f);
                result.push_back((0b11110 << 3) | ((codepoint >> 18) & 0x7));
                result.push_back((0b10 << 6) | ((codepoint >> 12) & 0x3F));
                result.push_back((0b10 << 6) | ((codepoint >> 6) & 0x3F));
                result.push_back((0b10 << 6) | (codepoint & 0x3F));
                continue;
            }
            default: llvm_unreachable("Should have errored in deduceByteCount");
        }
    }
    return result;
}

enum class ConstantPoolTag : std::uint8_t
{
    Class = 7,
    FieldRef = 9,
    MethodRef = 10,
    InterfaceMethodRef = 11,
    String = 8,
    Integer = 3,
    Float = 4,
    Long = 5,
    Double = 6,
    NameAndType = 12,
    Utf8 = 1,
    MethodHandle = 15,
    MethodType = 16,
    Dynamic = 17,
    InvokeDynamic = 18,
    Module = 19,
    Package = 20
};

ConstantPoolInfo parseConstantPoolInfo(llvm::ArrayRef<char>& bytes, llvm::StringSaver& stringSaver)
{
    auto tag = consume<ConstantPoolTag>(bytes);
    switch (tag)
    {
        case ConstantPoolTag::Class: return ClassInfo{consume<std::uint16_t>(bytes)};
        case ConstantPoolTag::FieldRef:
            return FieldRefInfo{consume<std::uint16_t>(bytes), consume<std::uint16_t>(bytes)};
        case ConstantPoolTag::MethodRef:
            return MethodRefInfo{consume<std::uint16_t>(bytes), consume<std::uint16_t>(bytes)};
        case ConstantPoolTag::InterfaceMethodRef:
            return InterfaceMethodRefInfo{consume<std::uint16_t>(bytes), consume<std::uint16_t>(bytes)};
        case ConstantPoolTag::String: return StringInfo{consume<std::uint16_t>(bytes)};
        case ConstantPoolTag::Integer: return IntegerInfo{consume<std::int32_t>(bytes)};
        case ConstantPoolTag::Float: return FloatInfo{consume<float>(bytes)};
        case ConstantPoolTag::Long: return LongInfo{consume<std::int64_t>(bytes)};
        case ConstantPoolTag::Double: return DoubleInfo{consume<double>(bytes)};
        case ConstantPoolTag::NameAndType:
            return NameAndTypeInfo{consume<std::uint16_t>(bytes), consume<std::uint16_t>(bytes)};
        case ConstantPoolTag::Utf8:
        {
            auto length = consume<std::uint16_t>(bytes);
            llvm::StringRef rawString = consumeRawString(length, bytes);
            return Utf8Info{stringSaver.save(toUTF8(rawString))};
        }
        case ConstantPoolTag::MethodHandle:
            return MethodHandleInfo{consume<MethodHandleInfo::Kind>(bytes), consume<std::uint16_t>(bytes)};
        case ConstantPoolTag::MethodType: return MethodTypeInfo{consume<std::uint16_t>(bytes)};
        case ConstantPoolTag::Dynamic: return DynamicInfo{consume<std::uint16_t>(bytes), consume<std::uint16_t>(bytes)};
        case ConstantPoolTag::InvokeDynamic:
            return InvokeDynamicInfo{consume<std::uint16_t>(bytes), consume<std::uint16_t>(bytes)};
        case ConstantPoolTag::Module: return ModuleInfo{consume<std::uint16_t>(bytes)};
        case ConstantPoolTag::Package: return PackageInfo{consume<std::uint16_t>(bytes)};
    }
    llvm_unreachable("Invalid tag");
}

std::pair<PoolIndex<Utf8Info>, llvm::ArrayRef<char>> parseAttributeInfo(llvm::ArrayRef<char>& bytes)
{
    auto nameIndex = consume<std::uint16_t>(bytes);
    auto length = consume<std::uint32_t>(bytes);
    auto raw = consumeRawString(length, bytes);
    return {nameIndex, {raw.begin(), raw.end()}};
}

template <FieldOrMethodInfo T>
T parseFieldOrMethodInfo(llvm::ArrayRef<char>& bytes, const ClassFile& classFile)
{
    auto accessFlags = consume<AccessFlag>(bytes);
    auto nameIndex = consume<std::uint16_t>(bytes);
    auto descriptorIndex = consume<std::uint16_t>(bytes);

    AttributeMap attributes;
    auto attributeCount = consume<std::uint16_t>(bytes);
    for (std::size_t i = 0; i < attributeCount; i++)
    {
        auto [name, attrBytes] = parseAttributeInfo(bytes);
        attributes.insert(name.resolve(classFile)->text, attrBytes);
    }
    return T(accessFlags, nameIndex, descriptorIndex, std::move(attributes));
}

} // namespace

jllvm::ClassFile jllvm::ClassFile::parseFromFile(llvm::ArrayRef<char> bytes, llvm::StringSaver& stringSaver)
{
    jllvm::ClassFile result;

    auto magic = consume<std::uint32_t>(bytes);
    if (magic != 0xCAFEBABE)
    {
        llvm::report_fatal_error("Error reading class file: Invalid file magic");
    }
    consume<std::uint16_t>(bytes); // major version
    consume<std::uint16_t>(bytes); // minor version

    auto constantPoolLength = consume<std::uint16_t>(bytes) - 1;
    result.m_constantPool.resize(constantPoolLength);
    for (std::size_t i = 0; i < constantPoolLength; i++)
    {
        result.m_constantPool[i] = parseConstantPoolInfo(bytes, stringSaver);
        if (holds_alternative<DoubleInfo>(result.m_constantPool[i])
            || holds_alternative<LongInfo>(result.m_constantPool[i]))
        {
            i++;
        }
    }
    result.m_accessFlags = consume<AccessFlag>(bytes);
    result.m_thisClass = consume<PoolIndex<ClassInfo>>(bytes).resolve(result)->nameIndex.resolve(result)->text;

    if (auto superClass = consume<PoolIndex<ClassInfo>>(bytes))
    {
        result.m_superClass = superClass.resolve(result)->nameIndex.resolve(result)->text;
    }

    result.m_interfaces.resize(consume<std::uint16_t>(bytes));
    std::generate(result.m_interfaces.begin(), result.m_interfaces.end(),
                  [&]()
                  { return consume<PoolIndex<ClassInfo>>(bytes).resolve(result)->nameIndex.resolve(result)->text; });

    result.m_fields.resize(consume<std::uint16_t>(bytes));
    std::generate(result.m_fields.begin(), result.m_fields.end(),
                  [&]() { return parseFieldOrMethodInfo<FieldInfo>(bytes, result); });

    result.m_methods.resize(consume<std::uint16_t>(bytes));
    std::generate(result.m_methods.begin(), result.m_methods.end(),
                  [&]() { return parseFieldOrMethodInfo<MethodInfo>(bytes, result); });

    auto attributeCount = consume<std::uint16_t>(bytes);
    for (std::size_t i = 0; i < attributeCount; i++)
    {
        auto [name, attrBytes] = parseAttributeInfo(bytes);
        result.m_attributes.insert(name.resolve(result)->text, attrBytes);
    }

    return result;
}

Code Code::parse(llvm::ArrayRef<char> bytes)
{
    Code result;
    result.m_maxStack = consume<std::uint16_t>(bytes);
    result.m_maxLocals = consume<std::uint16_t>(bytes);
    auto codeCount = consume<std::uint32_t>(bytes);
    auto rawString = consumeRawString(codeCount, bytes);
    result.m_code = llvm::ArrayRef(rawString.begin(), rawString.end());
    auto exceptionTableCount = consume<std::uint16_t>(bytes);
    result.m_exceptionTable.resize(exceptionTableCount);
    for (auto& iter : result.m_exceptionTable)
    {
        iter = {consume<std::uint16_t>(bytes), consume<std::uint16_t>(bytes), consume<std::uint16_t>(bytes),
                consume<PoolIndex<ClassInfo>>(bytes)};
    }
    return result;
}

llvm::StringRef FieldInfo::getName(const ClassFile& classFile) const
{
    return m_nameIndex.resolve(classFile)->text;
}

FieldType FieldInfo::getDescriptor(const jllvm::ClassFile& classFile) const
{
    return FieldType(m_descriptorIndex.resolve(classFile)->text);
}

llvm::StringRef MethodInfo::getName(const ClassFile& classFile) const
{
    return m_nameIndex.resolve(classFile)->text;
}

MethodType MethodInfo::getDescriptor(const jllvm::ClassFile& classFile) const
{
    return MethodType(m_descriptorIndex.resolve(classFile)->text);
}
