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

#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/StringSaver.h>

#include <jllvm/support/Variant.hpp>

#include <cstdint>
#include <tuple>
#include <vector>

#include <swl/variant.hpp>

#include "Descriptors.hpp"

namespace jllvm
{
struct ClassFile;

/// Convenience class for strong typing of indices within the constant pool of a class file.
/// Class files contain a lot of indices into the constant pool that are usually restricted to only ever being
/// one kind of constant. Since the constant pool is one large heterogeneous, manual access would require nasty manual
/// type casts. This convenience class nicely wraps all this complexity.
///
/// Internally this class is simply a 16 bit unsigned integer as that is the index type for the constant pool.
/// The template parameters must be one of the alternatives in 'ConstantPoolInfo'.
template <class First, class... Rest>
class PoolIndex
{
    friend class ClassFile;

    std::uint16_t m_index{};

public:
    /// Default constructed pool index. This never refers to any entries within the constant pool.
    PoolIndex() = default;

    /// Implicit constructor from a pool index.
    /*implicit*/ PoolIndex(std::uint16_t index) : m_index(index) {}

    /// Resolves the entry into 'classFile's constant pool.
    /// If this 'PoolIndex' may only ever point to one single type, it returns a pointer to that type.
    /// Else, if it may be one of multiple constant kinds, a variant of pointers to each is returned.
    decltype(auto) resolve(const ClassFile& classFile) const;

    /// Returns true if this pool index refers to an entry in a class files constant pool.
    explicit operator bool() const
    {
        return m_index != 0;
    }

    auto operator<=>(const PoolIndex& rhs) const = default;
};

struct Utf8Info;
struct NameAndTypeInfo;

/// Constant pool object representing a class name.
struct ClassInfo
{
    PoolIndex<Utf8Info> nameIndex;
};

/// Base class for constant pool objects representing references.
struct RefInfo
{
    PoolIndex<ClassInfo> classIndex;
    PoolIndex<NameAndTypeInfo> nameAndTypeIndex;
};

/// Constant pool object representing a reference to a field.
struct FieldRefInfo : RefInfo
{
};

/// Constant pool object representing a reference to a method.
struct MethodRefInfo : RefInfo
{
};

/// Constant pool object representing a reference to an interface method.
struct InterfaceMethodRefInfo : RefInfo
{
};

/// Constant pool object representing a Java string object.
struct StringInfo
{
    PoolIndex<Utf8Info> stringValue;
};

/// Constant pool object representing a 32 bit integer.
struct IntegerInfo
{
    std::int32_t value;
};

/// Constant pool object representing a single precision float.
struct FloatInfo
{
    float value;
};

/// Constant pool object representing a 64 bit integer.
struct LongInfo
{
    std::int64_t value;
};

/// Constant pool object representing a double precision float.
struct DoubleInfo
{
    double value;
};

/// Constant pool object representing a pair of a name and a descriptor.
/// This is usually used by the various reference object entries, where the name is the name of the object
/// and the descriptor represents the type. The descriptors can be parsed with the methods in the 'Descriptors.hpp'.
struct NameAndTypeInfo
{
    PoolIndex<Utf8Info> nameIndex;
    PoolIndex<Utf8Info> descriptorIndex;
};

/// Constant pool object representing a UTF-8 string.
struct Utf8Info
{
    llvm::StringRef text;
};

/// TODO: Document this once I understand its purpose.
struct MethodHandleInfo
{
    enum Kind : std::uint8_t
    {
        GetField = 1,
        GetStatic = 2,
        PutField = 3,
        PutStatic = 4,
        InvokeVirtual = 5,
        InvokeStatic = 6,
        InvokeSpecial = 7,
        NewInvokeSpecial = 8,
        InvokeInterface = 9
    } kind;
    PoolIndex<FieldRefInfo, MethodRefInfo, InterfaceMethodRefInfo> referenceIndex;
};

/// TODO: Document this once I understand its purpose.
struct MethodTypeInfo
{
    PoolIndex<Utf8Info> descriptorIndex;
};

/// TODO: Document this once I understand its purpose.
struct DynamicInfo
{
    std::uint16_t bootStrapMethodIndex;
    PoolIndex<NameAndTypeInfo> nameAndTypeIndex;
};

/// TODO: Document this once I understand its purpose.
struct InvokeDynamicInfo
{
    std::uint16_t bootStrapMethodIndex;
    PoolIndex<NameAndTypeInfo> nameAndTypeIndex;
};

/// TODO: Document this once I understand its purpose.
struct ModuleInfo
{
    PoolIndex<Utf8Info> nameIndex;
};

/// TODO: Document this once I understand its purpose.
struct PackageInfo
{
    PoolIndex<Utf8Info> packageInfo;
};

/// Variant of all the possible kinds of constant pool entries.
/// Note the 'std::monostate' is used because there are "empty" constant pool entries following
/// any 'LongInfo' entries and 'DoubleInfo' as required by the spec. These are always unreferenced however.
using ConstantPoolInfo =
    swl::variant<std::monostate, ClassInfo, FieldRefInfo, MethodRefInfo, InterfaceMethodRefInfo, StringInfo,
                 IntegerInfo, FloatInfo, LongInfo, DoubleInfo, NameAndTypeInfo, Utf8Info, MethodHandleInfo,
                 MethodTypeInfo, DynamicInfo, InvokeDynamicInfo, ModuleInfo, PackageInfo>;

/// TODO: Document each flag once they're all clearer.
enum class AccessFlag : std::uint16_t
{
    None = 0,
    Public = 0x0001,
    Private = 0x0002,
    Protected = 0x0004,
    Static = 0x0008,
    Final = 0x0010,
    Super = 0x0020,
    Bridge = 0x0040,
    Varargs = 0x0080,
    Native = 0x0100,
    Interface = 0x0200,
    Abstract = 0x0400,
    Strict = 0x0800,
    Synthetic = 0x1000,
    Annotation = 0x2000,
    Enum = 0x4000,
    Module = 0x8000,
    LLVM_MARK_AS_BITMASK_ENUM(Module)
};

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// Convenience class for accessing the attributes of an entity.
/// This is essentially a map of attribute names to the attributes themselves.
///
/// Attributes are contained in their unparsed raw byte form within the map
/// and only deserialized on lookup. See 'find' for more details.
class AttributeMap
{
    using AttributePointer = std::unique_ptr<void, void (*)(void*)>;
    mutable llvm::DenseMap<llvm::StringRef, std::pair<llvm::ArrayRef<char>, AttributePointer>> m_map;

public:
    AttributeMap() = default;
    ~AttributeMap() = default;
    AttributeMap(const AttributeMap&) = delete;
    AttributeMap& operator=(const AttributeMap&) = delete;
    AttributeMap(AttributeMap&&) noexcept = default;
    AttributeMap& operator=(AttributeMap&&) noexcept = default;

    void insert(llvm::StringRef name, llvm::ArrayRef<char> bytes)
    {
        m_map.try_emplace(name, bytes, AttributePointer(nullptr, nullptr));
    }

    /// Looks up an attribute in the attribute map and parses it if present.
    ///
    /// Attributes are represented by types which are required to have following structure:
    ///     * a static constexpr string called 'identifier' which is the name of the attribute
    ///     * a static 'parse(ArrayRef<char>, const ClassFile&)' method which returns a parsed instance of the attribute
    ///       class.
    ///
    /// 'T' of this method must be such a class. If the attribute is not present within the map a null pointer is
    /// returned.
    template <class T>
    T* find() const
    {
        auto result = m_map.find(T::identifier);
        if (result == m_map.end())
        {
            return nullptr;
        }

        if (!result->second.second)
        {
            result->second.second = std::unique_ptr<void, void (*)(void*)>(
                new T(T::parse(result->second.first)), +[](void* pointer) { delete reinterpret_cast<T*>(pointer); });
        }
        return reinterpret_cast<T*>(result->second.second.get());
    }
};

/// 'Code' attribute attached to methods containing the JVM Bytecode.
class Code
{
public:
    constexpr static llvm::StringRef identifier = "Code";

    static Code parse(llvm::ArrayRef<char> bytes);

    /// Exception table entry. Used to mark a range of JVM Bytecode instructions as "guarded" by an exception handler.
    /// Note that order of these is significant.
    struct ExceptionTable
    {
        /// Offset of the first op in 'code' that is guarded by this exception handler.
        std::uint16_t startPc{};
        /// Offset of the first op in 'code' that is no longer guarded by this exception handler.
        std::uint16_t endPc{};
        /// Offset to the exception handler executed if an exception with matching type was thrown.
        std::uint16_t handlerPc{};
        /// Index into the pool representing the class objects whose instances can be caught by this handler.
        PoolIndex<ClassInfo> catchType{};
    };

private:
    std::uint16_t m_maxStack;
    std::uint16_t m_maxLocals;
    llvm::ArrayRef<char> m_code;

    std::vector<ExceptionTable> m_exceptionTable;

    // Attributes follow, don't think we need those right now.

public:
    /// Returns the maximum size the operand stack required by the bytecode.
    std::uint16_t getMaxStack() const
    {
        return m_maxStack;
    }

    /// Returns the maximum amount of locals required by the bytecode.
    std::uint16_t getMaxLocals() const
    {
        return m_maxLocals;
    }

    /// Returns the serialized JVM bytecode of the containing method.
    llvm::ArrayRef<char> getCode() const
    {
        return m_code;
    }

    /// Returns the exception table of the containing method.
    llvm::ArrayRef<ExceptionTable> getExceptionTable() const
    {
        return m_exceptionTable;
    }
};

/// Info object of a field of the class represented by the class file.
class FieldInfo
{
    AccessFlag m_accessFlags{};
    PoolIndex<Utf8Info> m_nameIndex{};
    PoolIndex<Utf8Info> m_descriptorIndex{};
    AttributeMap m_attributes;

public:
    FieldInfo(AccessFlag accessFlags, PoolIndex<Utf8Info> nameIndex, PoolIndex<Utf8Info> descriptorIndex,
              AttributeMap&& attributes)
        : m_accessFlags(accessFlags),
          m_nameIndex(nameIndex),
          m_descriptorIndex(descriptorIndex),
          m_attributes(std::move(attributes))
    {
    }

    /// Returns true if this field is static.
    bool isStatic() const
    {
        return (m_accessFlags & AccessFlag::Static) != AccessFlag::None;
    }

    /// Returns the name of this field.
    llvm::StringRef getName(const ClassFile& classFile) const;

    /// Returns the field descriptor of this field, indicating its type.
    FieldType getDescriptor(const ClassFile& classFile) const;

    /// Returns the attributes of this field.
    const AttributeMap& getAttributes() const
    {
        return m_attributes;
    }

    /// Returns the access flags of this field.
    AccessFlag getAccessFlags() const
    {
        return m_accessFlags;
    }
};

/// Info object of a method of the class represented by the class file.
class MethodInfo
{
    AccessFlag m_accessFlags{};
    PoolIndex<Utf8Info> m_nameIndex{};
    PoolIndex<Utf8Info> m_descriptorIndex{};
    AttributeMap m_attributes;

public:
    MethodInfo(AccessFlag accessFlags, PoolIndex<Utf8Info> nameIndex, PoolIndex<Utf8Info> descriptorIndex,
               AttributeMap&& attributes)
        : m_accessFlags(accessFlags),
          m_nameIndex(nameIndex),
          m_descriptorIndex(descriptorIndex),
          m_attributes(std::move(attributes))
    {
    }

    /// Returns true if this method is final.
    bool isFinal() const
    {
        return (m_accessFlags & AccessFlag::Final) != AccessFlag::None;
    }

    /// Returns true if this method is static.
    bool isStatic() const
    {
        return (m_accessFlags & AccessFlag::Static) != AccessFlag::None;
    }

    /// Returns true if this method is native.
    bool isNative() const
    {
        return (m_accessFlags & AccessFlag::Native) != AccessFlag::None;
    }

    /// Returns true if this method is abstract.
    bool isAbstract() const
    {
        return (m_accessFlags & AccessFlag::Abstract) != AccessFlag::None;
    }

    /// Returns true if this method requires a VTable slot.
    bool needsVTableSlot(const ClassFile& classFile) const
    {
        return !isPrivate() && !isFinal() && !isStatic() && getName(classFile) != "<init>";
    }

    /// Returns the name of this method.
    llvm::StringRef getName(const ClassFile& classFile) const;

    /// Returns the method descriptor of this method, indicating its type.
    MethodType getDescriptor(const ClassFile& classFile) const;

    /// Returns the attributes of this method.
    const AttributeMap& getAttributes() const
    {
        return m_attributes;
    }

    bool isPrivate() const
    {
        return (m_accessFlags & AccessFlag::Private) != AccessFlag::None;
    }

    bool isPublic() const
    {
        return (m_accessFlags & AccessFlag::Public) != AccessFlag::None;
    }

    bool isProtected() const
    {
        return (m_accessFlags & AccessFlag::Protected) != AccessFlag::None;
    }
};

/// Top level struct representing a class file.
class ClassFile
{
    std::vector<ConstantPoolInfo> m_constantPool;
    AccessFlag m_accessFlags;
    PoolIndex<ClassInfo> m_thisClass;
    PoolIndex<ClassInfo> m_superClass;
    std::vector<llvm::StringRef> m_interfaces;
    std::vector<FieldInfo> m_fields;
    std::vector<MethodInfo> m_methods;
    AttributeMap m_attributes;

    template <class, class...>
    friend class PoolIndex;

public:
    /// Parses a class file from 'bytes'. 'stringSaver' is used additionally to manage the lifetimes of any strings
    /// created during parsing. This is currently used for UTF-8 constant pool entries.
    /// Note: The returned class file contains references into the underlying array of 'bytes' and must outlive
    /// its backing storage.
    static ClassFile parseFromFile(llvm::ArrayRef<char> bytes, llvm::StringSaver& stringSaver);

    /// Returns the name of the class defined by this class file.
    llvm::StringRef getThisClass() const;

    /// Returns the name of the super class of this class. Note, this is a null entry for java/lang/Object.
    std::optional<llvm::StringRef> getSuperClass() const;

    /// Returns the interfaces implemented by this class.
    llvm::ArrayRef<llvm::StringRef> getInterfaces() const
    {
        return m_interfaces;
    }

    /// Returns true if this class file defines an interface.
    bool isInterface() const
    {
        return (m_accessFlags & AccessFlag::Interface) != AccessFlag::None;
    }

    /// Returns true if this class file defines an abstract class.
    bool isAbstract() const
    {
        return (m_accessFlags & AccessFlag::Abstract) != AccessFlag::None;
    }

    /// Returns true if the super flag, used to modify the behaviour of 'invokespecial', is set.
    bool hasSuperFlag() const
    {
        return (m_accessFlags & AccessFlag::Super) != AccessFlag::None;
    }

    /// Returns the fields of this class.
    llvm::ArrayRef<FieldInfo> getFields() const
    {
        return m_fields;
    }

    /// Returns the methods of this class.
    llvm::ArrayRef<MethodInfo> getMethods() const
    {
        return m_methods;
    }

    /// Returns the attributes of this class.
    const AttributeMap& getAttributes() const
    {
        return m_attributes;
    }
};

template <class First, class... Rest>
decltype(auto) PoolIndex<First, Rest...>::resolve(const jllvm::ClassFile& classFile) const
{
    using Result = std::conditional_t<(sizeof...(Rest) > 0), swl::variant<const First*, const Rest*...>, const First*>;
    return jllvm::match(classFile.m_constantPool[m_index - 1],
                        [](const auto& alt) -> Result
                        {
                            if constexpr (std::is_convertible_v<std::decay_t<decltype(alt)>*, Result>)
                            {
                                return &alt;
                            }
                            llvm_unreachable("Unexpected alternative");
                        });
}

} // namespace jllvm
