
#pragma once

#include <llvm/ADT/StringRef.h>

#include <memory>
#include <variant>
#include <vector>

namespace jllvm
{

/// <BaseType> ::= 'B' | 'C' | 'D' | 'F' | 'I' | 'J' | 'S' | 'Z'
/// Note: We add 'V' for void here as well for convenience.
enum class BaseType
{
    Byte, /// 'B'
    Char, /// 'C'
    Double, /// 'D'
    Float, /// 'F'
    Int, /// 'I'
    Long, /// 'J'
    Short, /// 'S'
    Boolean, /// 'Z'
    Void /// 'V'
};

/// <ObjectType> ::= 'L' <ClassName> ';'
struct ObjectType
{
    llvm::StringRef className;

    bool operator==(const ObjectType& rhs) const
    {
        return className == rhs.className;
    }

    bool operator!=(const ObjectType& rhs) const
    {
        return !(rhs == *this);
    }
};

struct ArrayType;

using FieldType = std::variant<BaseType, ObjectType, ArrayType>;

/// <ArrayType> ::= '[' <FieldType>
struct ArrayType
{
    std::unique_ptr<FieldType> componentType;

    bool operator==(const ArrayType& rhs) const
    {
        return *componentType == *rhs.componentType;
    }

    bool operator!=(const ArrayType& rhs) const
    {
        return !(rhs == *this);
    }
};

/// Parses a field descriptor string to a more convenient object representation.
/// The lifetime of any strings (basically any contained 'ObjectType's) is equal to the lifetime of the string passed
/// in.
/// Note: This function does not allow error handling at this point in time and either exhibits UB or asserts
/// on invalid strings.
FieldType parseFieldType(llvm::StringRef string);

inline bool isReferenceDescriptor(llvm::StringRef string)
{
    return string.front() == 'L' || string.front() == '[';
}

/// <MethodType> ::= '(' { <FieldType> } ')' <FieldType>
struct MethodType
{
    std::vector<FieldType> parameters;
    FieldType returnType;
};

/// Parses a method descriptor string to a more convenient object representation.
/// Same notes about lifetimes and error handling apply as in 'parseFieldType'.
MethodType parseMethodType(llvm::StringRef string);

} // namespace jllvm
