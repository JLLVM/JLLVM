#pragma once

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
    std::int32_t hashCode;
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

class Object;

/// In memory representation of Java array with component type 'T'.
/// 'T' is always either a primitive or a pointer to Java objects.
template <class T = Object*>
class Array : public ObjectInterface
{
    ObjectHeader m_header;
    std::uint32_t m_length;
    // GCC and Clang extension allowing to place and index into an array placed right after the object
    // without introducing any padding inbetween.
    T m_trailing[];

    Array(ObjectHeader header, std::uint32_t length) : m_header{header}, m_length{length} {}

public:
    /// Function to create a new array object. The array object is allocated within 'allocator'
    /// with 'classObject' as the corresponding array class object.
    /// 'length' is the amount entries in the resulting array.
    static Array* create(llvm::BumpPtrAllocator& allocator, const ClassObject* classObject, std::uint32_t length)
    {
        return new (allocator.Allocate(arrayElementsOffset() + sizeof(T[length]), alignof(Array)))
            Array({classObject, 0}, length);
    }

    /// Returns the byte offset from the start of the Array object to the first array element.
    constexpr static std::size_t arrayElementsOffset()
    {
        return offsetof(Array<T>, m_trailing);
    }

    /// Returns the length of the array.
    std::uint32_t getLength() const
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

/// In memory representation for a general Java object.
class Object : public ObjectInterface
{
    ObjectHeader m_header;
};

static_assert(std::is_standard_layout_v<Object>);

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
        : m_header{classObject, 0}, m_value{value}, m_coder{coder}
    {
    }

    Array<std::uint8_t>& getValue()
    {
        return *m_value;
    }
};

static_assert(std::is_standard_layout_v<String>);

} // namespace jllvm
