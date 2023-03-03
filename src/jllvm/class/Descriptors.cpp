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
