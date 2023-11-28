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

#include "ClassObjectStubMangling.hpp"

std::string jllvm::mangleDirectMethodCall(llvm::StringRef className, llvm::StringRef methodName, MethodType descriptor)
{
    return (className + "." + methodName + ":" + descriptor.textual()).str();
}

std::string jllvm::mangleDirectMethodCall(const MethodInfo& methodInfo, const ClassFile& classFile)
{
    llvm::StringRef className = classFile.getThisClass();
    llvm::StringRef methodName = methodInfo.getName(classFile);
    MethodType descriptor = methodInfo.getDescriptor(classFile);

    return mangleDirectMethodCall(className, methodName, descriptor);
}

std::string jllvm::mangleDirectMethodCall(const jllvm::Method* method)
{
    return mangleDirectMethodCall(method->getClassObject()->getClassName(), method->getName(), method->getType());
}

std::string jllvm::mangleOSRMethod(const jllvm::Method* method, unsigned offset)
{
    return mangleDirectMethodCall(method) + "$" + std::to_string(offset);
}

std::string jllvm::mangleFieldAccess(llvm::StringRef className, llvm::StringRef fieldName, FieldType descriptor)
{
    return (className + "." + fieldName + ":" + descriptor.textual()).str();
}

constexpr llvm::StringLiteral virtualCallPrefix = "Virtual Call to ";
constexpr llvm::StringLiteral interfaceCallPrefix = "Interface Call to ";

std::string jllvm::mangleMethodResolutionCall(MethodResolution resolution, llvm::StringRef className,
                                              llvm::StringRef methodName, MethodType descriptor)
{
    std::string directMethodMangling = mangleDirectMethodCall(className, methodName, descriptor);
    switch (resolution)
    {
        case MethodResolution::Virtual: return (virtualCallPrefix + directMethodMangling).str();
        case MethodResolution::Interface: return (interfaceCallPrefix + directMethodMangling).str();
    }
    LLVM_BUILTIN_UNREACHABLE;
}

constexpr llvm::StringLiteral specialCallPrefix = "Special Call to ";
constexpr llvm::StringLiteral specialCallInfix = ":from ";

std::string jllvm::mangleSpecialMethodCall(llvm::StringRef className, llvm::StringRef methodName, MethodType descriptor,
                                           std::optional<FieldType> callerClass)
{
    return (specialCallPrefix + mangleDirectMethodCall(className, methodName, descriptor)
            + (callerClass ? specialCallInfix + callerClass->textual() : ""))
        .str();
}

constexpr llvm::StringLiteral staticCallPrefix = "Static Call to ";

std::string jllvm::mangleStaticCall(llvm::StringRef className, llvm::StringRef methodName, MethodType descriptor)
{
    return (staticCallPrefix + mangleDirectMethodCall(className, methodName, descriptor)).str();
}

constexpr llvm::StringLiteral classObjectPrefix = "Load ";

std::string jllvm::mangleClassObjectAccess(FieldType descriptor)
{
    return (classObjectPrefix + descriptor.textual()).str();
}

std::string jllvm::mangleClassObjectGlobal(FieldType descriptor)
{
    return descriptor.textual();
}

std::string jllvm::mangleMethodGlobal(const Method* method)
{
    return '&' + mangleDirectMethodCall(method);
}

constexpr llvm::StringLiteral globalStringPrefix = "'";

std::string jllvm::mangleStringGlobal(llvm::StringRef contents)
{
    return globalStringPrefix.data() + contents.str();
}

jllvm::DemangledVariant jllvm::demangleStubSymbolName(llvm::StringRef symbolName)
{
    bool isStatic = false;
    bool isClassObjectLoad = false;
    bool isSpecialMethod = false;
    std::optional<MethodResolution> resolution;

    if (symbolName.consume_front(globalStringPrefix))
    {
        return DemangledStringGlobal{symbolName};
    }

    if (symbolName.consume_front(classObjectPrefix))
    {
        isClassObjectLoad = true;
    }
    else if (symbolName.consume_front(staticCallPrefix))
    {
        isStatic = true;
    }
    else if (symbolName.consume_front(virtualCallPrefix))
    {
        resolution = MethodResolution::Virtual;
    }
    else if (symbolName.consume_front(interfaceCallPrefix))
    {
        resolution = MethodResolution::Interface;
    }
    else if (symbolName.consume_front(specialCallPrefix))
    {
        isSpecialMethod = true;
    }

    // Find the string part prior to the dot.
    llvm::StringRef className = symbolName.take_until([](char c) { return c == '.'; });
    symbolName = symbolName.drop_front(className.size());
    // If the name is now empty, then there was no dot in the string. This can only be a
    // class object load if a valid 'FieldType'.
    if (symbolName.empty())
    {
        if (!FieldType::verify(className))
        {
            return std::monostate{};
        }
        if (isClassObjectLoad)
        {
            return DemangledLoadClassObject{FieldType(className)};
        }
        return DemangledClassObjectGlobal{FieldType(className)};
    }

    // Class object load cannot contain a dot.
    if (isClassObjectLoad)
    {
        return std::monostate{};
    }

    symbolName = symbolName.drop_front();

    llvm::StringRef name = symbolName.take_until([](char c) { return c == ':'; });
    symbolName = symbolName.drop_front(name.size());
    // There had to have been a colon.
    if (symbolName.empty())
    {
        return std::monostate{};
    }
    // Remaining part of the symbol is now either the method or field descriptor.
    // Dispatch to either group depending on whether we saw a corresponding prefix.
    symbolName = symbolName.drop_front();
    if (isStatic || resolution)
    {
        if (!MethodType::verify(symbolName))
        {
            return std::monostate{};
        }
        if (isStatic)
        {
            return DemangledStaticCall{className, name, MethodType(symbolName)};
        }
        return DemangledMethodResolutionCall{*resolution, className, name, MethodType(symbolName)};
    }

    if (isSpecialMethod)
    {
        llvm::StringRef methodType = symbolName.take_until([](char c) { return c == ':'; });
        symbolName = symbolName.drop_front(methodType.size());
        if (symbolName.empty())
        {
            // Special method call did not have a caller appended at the back.
            if (!MethodType::verify(methodType))
            {
                return std::monostate{};
            }
            return DemangledSpecialCall{className, name, MethodType(methodType), std::nullopt};
        }
        if (!symbolName.consume_front(specialCallInfix))
        {
            return std::monostate{};
        }
        if (!FieldType::verify(symbolName))
        {
            return std::monostate{};
        }
        return DemangledSpecialCall{className, name, MethodType(methodType), FieldType(symbolName)};
    }

    if (FieldType::verify(symbolName))
    {
        return DemangledFieldAccess{className, name, FieldType(symbolName)};
    }
    return std::monostate{};
}
