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

#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/Support/Allocator.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "jllvm/support/Encoding.hpp"

namespace jllvm
{
class ClassObject;

/// Object header that every Java object starts with. Contains the class object.
/// Purpose of this being its own type is mostly size calculations.
struct ObjectHeader
{
    /// Type of the object.
    const ClassObject* classObject;
    /// Cached hash of the object. This has to be stored and lazily calculated on first use. We cannot use an objects
    /// address as we have a relocating garbage collector. It is therefore unstable.
    /// A value of 0 indicates the hashCode of an object has not yet been calculated.
    std::int32_t hashCode = 0;

    /// Initialize an object header with the objects class object.
    explicit ObjectHeader(const ClassObject* classObject) : classObject(classObject) {}
};

/// Pure interface class used to implement methods one would commonly associate with 'Object'.
/// We cannot use C++ inheritance to do this as that does not have a defined memory layout. We instead use
/// composition and require all Java objects to simply always start with an 'ObjectHeader' instance.
/// Inheriting from this class when that is not the case is undefined behaviour.
class ObjectInterface
{
public:
    /// Returns the object header of the class.
    ObjectHeader& getObjectHeader()
    {
        return *reinterpret_cast<ObjectHeader*>(this);
    }

    const ObjectHeader& getObjectHeader() const
    {
        return *reinterpret_cast<const ObjectHeader*>(this);
    }

    /// Returns true if this object is an instance of 'classObject'.
    bool instanceOf(const ClassObject* classObject) const;

    /// Returns the class object of this java object.
    const ClassObject* getClass() const
    {
        return getObjectHeader().classObject;
    }
};

/// In memory representation for a general Java object.
class Object : public ObjectInterface
{
    ObjectHeader m_header;

public:
    explicit Object(const ClassObject* classObject) : m_header(classObject) {}
};

static_assert(std::is_standard_layout_v<Object>);

/// Concept for any type that is compatible with Java objects in their object representation.
/// This should be used in places when doing interop that require the storage/value to be identical to the corresponding
/// Java type.
template <class T>
concept JavaCompatible =
    std::is_arithmetic_v<T> || std::is_void_v<T> || std::is_base_of_v<ObjectInterface, std::remove_pointer_t<T>>;

/// In memory representation of Java array with component type 'T'.
/// 'T' is always either a primitive or a pointer to Java objects.
template <JavaCompatible T = ObjectInterface*>
class Array : public ObjectInterface
{
    ObjectHeader m_header;
    std::uint32_t m_length;
    // GCC and Clang extension allowing to place and index into an array placed right after the object
    // without introducing any padding inbetween.
    T m_trailing[];

public:
    Array(const ClassObject* classObject, std::uint32_t length) : m_header{classObject}, m_length{length} {}

    using value_type = T;

    /// Function to create a new array object. The array object is allocated within 'allocator'
    /// with 'classObject' as the corresponding array class object.
    /// 'length' is the amount entries in the resulting array.
    static Array* create(llvm::BumpPtrAllocator& allocator, const ClassObject* classObject, std::uint32_t length)
    {
        return new (allocator.Allocate(arrayElementsOffset() + sizeof(T) * length, alignof(Array)))
            Array(classObject, length);
    }

    /// Returns the byte offset from the start of the Array object to the first array element.
    constexpr static std::size_t arrayElementsOffset()
    {
        return offsetof(Array<T>, m_trailing);
    }

    /// Returns the length of the array.
    std::uint32_t size() const
    {
        return m_length;
    }

    /// Returns the array element with the given index.
    T& operator[](std::uint32_t index)
    {
        assert(index < m_length);
        return m_trailing[index];
    }

    T operator[](std::uint32_t index) const
    {
        assert(index < m_length);
        return m_trailing[index];
    }

    /// Returns a pointer to the array storage.
    T* data()
    {
        return m_trailing;
    }

    const T* data() const
    {
        return m_trailing;
    }

    /// Returns the begin iterator of the array.
    T* begin()
    {
        return m_trailing;
    }

    const T* begin() const
    {
        return m_trailing;
    }

    /// Returns the end iterator of the array.
    T* end()
    {
        return m_trailing + m_length;
    }

    const T* end() const
    {
        return m_trailing + m_length;
    }
};

static_assert(std::is_standard_layout_v<Array<>>);

/// In memory representation for a Java String.
class String : public ObjectInterface
{
    ObjectHeader m_header;
    Array<std::uint8_t>* m_value;
    std::uint8_t m_coder;
    std::int32_t m_hash{};
    bool m_hashIsZero{true};

public:
    String(const ClassObject* classObject, Array<std::uint8_t>* value, std::uint8_t coder)
        : m_header{classObject}, m_value{value}, m_coder{coder}
    {
    }

    Array<std::uint8_t>& getValue()
    {
        return *m_value;
    }

    std::string toUTF8() const
    {
        return fromJavaCompactEncoding({{m_value->data(), m_value->size()}, CompactEncoding{m_coder}});
    }
};

static_assert(std::is_standard_layout_v<String>);

/// In memory representation for a Java Throwable.
struct Throwable : ObjectInterface
{
    ObjectHeader header;

    Object* backtrace = nullptr;
    String* detailMessage = nullptr;
    Throwable* cause = nullptr;
    Array<Object*>* stackTrace = nullptr;
    std::int32_t depth{};
    Object* suppressedExceptions = nullptr;

    explicit Throwable(const ClassObject* classObject) : header(classObject) {}
};

static_assert(std::is_standard_layout_v<Throwable>);

// Specified here https://docs.oracle.com/en/java/javase/17/docs/specs/jvmti.html#GetThreadState
enum class ThreadState : std::int32_t
{
    Alive = 0x1,
    Terminated = 0x2,
    Runnable = 0x4,
    BlockedOnMonitorEnter = 0x400,
    Waiting = 0x80,
    WaitingIndefinitely = 0x10,
    WaitingWithTimeout = 0x20,
    Sleeping = 0x40,
    InObjectWait = 0x100,
    Parked = 0x200,
    Suspended = 0x100000,
    Interrupted = 0x200000,
    InNative = 0x400000,
    LLVM_MARK_AS_BITMASK_ENUM(InNative)
};
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

struct Reference : ObjectInterface
{
    ObjectHeader header;

    Object* referent{};
};

static_assert(std::is_standard_layout_v<Reference>);

} // namespace jllvm
