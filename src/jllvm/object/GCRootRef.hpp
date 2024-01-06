// Copyright (C) 2024 The JLLVM Contributors.
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

#include "Object.hpp"

namespace jllvm
{

/// CRTP trait allowing injecting extra methods into 'GCRootRef' based on its template parameter.
template <class Self>
struct GCRootRefTrait
{
};

/// Class used to refer to a so called "root" allocated by a 'RootFreeList', usually by the garbage collectors.
/// This class can be seen as a GC-Safe equivalent to a pointer for C++ code, allowing one to refer to an object
/// and continue referring to the object even after the object has been relocated by the garbage collector.
/// It also has the important role of keeping the object it refers to alive, preventing it from being deleted by the
/// garbage collector.
///
/// Most importantly, this class does not manage the lifetime of the root but relies on an outside mechanism to keep
/// the underlying root alive.
/// Rather its a lightweight reference type that should be passed by value, analogous to what 'std::string_view'
/// is to 'std::string' (the equivalent of 'std::string' being 'GCUniqueRoot' in this analogy).
/// The primary mechanism to create roots are by using the 'root' method of the 'GarbageCollector'.
///
/// See its documentation as to when to use 'GCRootRef', 'GCUniqueRoot' or a plain 'T*'.
///
/// 'T' is the memory layout of the Java object that should be used when retrieving it.
template <JavaObject T = ObjectInterface>
class GCRootRef : public GCRootRefTrait<GCRootRef<T>>
{
    ObjectInterface** m_root = nullptr;

    template <JavaObject U>
    friend class GCRootRef;

    T* get() const
    {
        if (!hasRoot())
        {
            return nullptr;
        }
        return static_cast<T*>(*m_root);
    }

public:
    /// Creates a new 'GCRootRef' from a root. The lifetime of the root must be valid throughout the use of the
    /// 'GCRootRef'.
    explicit GCRootRef(ObjectInterface** root) : m_root(root) {}

    /// Creates a 'GCRootRef' with no root. 'GCRootRef' without roots are for all intents and purposes equal to
    /// 'GCRootRef's that refer to a null reference with the exception of NOT being able to assign an object to it.
    /// Prefer assignment from another 'GCRootRef' instead.
    GCRootRef() = default;

    /// Allows implicit construction of a 'GCRootRef' without a root from a nullptr.
    GCRootRef(std::nullptr_t) : GCRootRef() {}

    /// Creates a new 'GCRootRef' from a derived 'GCRootRef', referring to the same root.
    template <std::derived_from<T> U>
    GCRootRef(GCRootRef<U> rhs) requires(!std::is_same_v<T, U>) : m_root(rhs.m_root)
    {
    }

    /// Assignment operator from a derived 'GCRootRef'.
    template <std::derived_from<T> U>
    GCRootRef& operator=(GCRootRef<U> rhs) requires(!std::is_same_v<T, U>)
    {
        *this = static_cast<GCRootRef<T>>(rhs);
        return *this;
    }

    /// Explicit cast to a 'GCRootRef' of another type. This allows both up and down casting and does not perform any
    /// validity checks. It is therefore up to the user to make sure the cast is valid.
    template <class U>
    explicit operator GCRootRef<U>() const
    {
        return GCRootRef<U>(m_root);
    }

    /// Explicit cast to 'T*'. This operation should generally be avoided in favour of just using the 'GCRootRef' as
    /// intended.
    explicit operator T*() const
    {
        return get();
    }

    /// Returns true if this 'GCRootRef' has a root.
    bool hasRoot() const
    {
        return m_root;
    }

    /// Assign an object to the root of this 'GCRootRef'. This is only a valid operation if the 'GCRootRef' is not
    /// empty i.e. refers to a root.
    void assign(T* object)
    {
        assert(hasRoot() && "GCRootRef must have a root");
        *m_root = const_cast<std::remove_const_t<T>*>(object);
    }

    /// Returns true if 'lhs' and 'rhs' refer to the same object.
    template <class U>
    friend bool operator==(GCRootRef<T> lhs, GCRootRef<U> rhs)
    {
        return lhs.get() == static_cast<U*>(rhs);
    }

    friend bool operator==(GCRootRef<T> lhs, const T* rhs)
    {
        return lhs.get() == rhs;
    }

    /// Returns true if this root contains a reference to an object.
    explicit operator bool() const
    {
        return get();
    }

    /// Returns true if this root does not contain a reference to an object.
    bool operator!() const
    {
        return !static_cast<bool>(*this);
    }

    /// Allows using a 'GCRootRef<T>' with the same syntax as you would a 'T*'
    T* operator->() const
    {
        return get();
    }

    T& operator*() const
    {
        return *get();
    }

    /// Returns the address of the Java object.
    /// The address is only valid until the next garbage collection.
    T* address() const
    {
        return get();
    }

    /// Returns the underlying root referred to by this 'GCRootRef'.
    ObjectInterface** data() const
    {
        return m_root;
    }
};

/// Trait for 'GCRootRef's of array type. Adds a GC safe indexing operator and iterators.
template <JavaCompatible T>
struct GCRootRefTrait<GCRootRef<Array<T>>>
{
    using Self = GCRootRef<Array<T>>;

    /// Proxy object representing an element within an array.
    /// Assigning or reading the element is GC safe at all times.
    class GCRootArrayElementRef
    {
        Self m_root;
        std::size_t m_index;

    public:
        GCRootArrayElementRef(Self root, std::size_t index) : m_root(root), m_index(index) {}

        operator T() const
        {
            return (*m_root)[m_index];
        }

        const GCRootArrayElementRef& operator=(T value) const
        {
            // Assign to the actual underlying array object.
            (*m_root)[m_index] = value;
            return *this;
        }
    };

    class iterator : public llvm::indexed_accessor_iterator<iterator, Self, GCRootArrayElementRef>
    {
        using Base = llvm::indexed_accessor_iterator<iterator, Self, GCRootArrayElementRef>;

    public:
        iterator(Self base, ptrdiff_t index) : Base(base, index) {}

        GCRootArrayElementRef operator*() const
        {
            return this->getBase()[this->getIndex()];
        }
    };

    /// Returns an iterator to the first element in the array.
    iterator begin() const
    {
        return iterator(static_cast<const Self&>(*this), 0);
    }

    /// Returns an iterator past the last element in the array.
    iterator end() const
    {
        return iterator(static_cast<const Self&>(*this), static_cast<const Self&>(*this)->size());
    }

    /// Accesses an element in the array with the given index.
    /// Reading the element or assigning to it is safe against any garbage collection.
    GCRootArrayElementRef operator[](std::size_t index) const
    {
        return GCRootArrayElementRef(static_cast<const Self&>(*this), index);
    }
};

/// Adaptor class for functions to accept both 'T*' and 'GCRootRef<T>' parameters.
/// It does so by supporting implicit conversions from both of these types and implicit conversion to 'T*'.
template <JavaObject T>
class GCRootRefOrPointer
{
    T* m_pointer;

public:
    /// Implicit construction from 'T*'. Makes it possible for the compiler to deduce 'T' as well.
    /*implicit*/ GCRootRefOrPointer(T* pointer) : m_pointer(pointer) {}

    /// Implicit construction from a pointer to a type derived from 'T'.
    template <std::derived_from<T> U>
    /*implicit*/ GCRootRefOrPointer(U* pointer) requires(!std::same_as<T, U>) : m_pointer(pointer)
    {
    }

    /// Implicit construction from 'GCRootRef<T>'. Makes it possible for the compiler to deduce 'T' as well.
    /*implicit*/ GCRootRefOrPointer(GCRootRef<T> ref) : m_pointer(ref.address()) {}

    /// Implicit construction from a 'GCRootRef' to a type derived from 'T'.
    template <std::derived_from<T> U>
    /*implicit*/ GCRootRefOrPointer(GCRootRef<U> ref) requires(!std::same_as<T, U>) : m_pointer(ref.address())
    {
    }

    /// Implicit construction from another 'GCRootRefOrPointer' to a type derived from 'T'.
    template <std::derived_from<T> U>
    /*implicit*/ GCRootRefOrPointer(GCRootRefOrPointer<U> ref) requires(!std::same_as<T, U>) : m_pointer(ref)
    {
    }

    /// Allows using pointer member access syntax.
    T* operator->() const
    {
        return m_pointer;
    }

    /// Implicitly convert to 'T*'.
    operator T*() const
    {
        return m_pointer;
    }
};

/// Adaptor class for returning a 'T*&'.
/// Makes it possible to also assign a 'GCRootRef' to the pointer.
template <JavaObject T>
class ObjectPointerRef
{
    T*& m_reference;

public:
    /// Constructs 'ObjectPointerRef' from a 'T*&'.
    explicit ObjectPointerRef(T*& pointer) : m_reference(pointer) {}

    /// Implicit conversion to 'T*' for reading.
    operator T*() const
    {
        return m_reference;
    }

    /// Implicit conversion to any 'GCRootRefOrPointer' of a super type of 'T'.
    template <class U>
    operator GCRootRefOrPointer<U>() const requires std::is_base_of_v<U, T>
    {
        return m_reference;
    }

    /// Allow assignment to the reference from any super type of 'T'.
    template <class U>
    const ObjectPointerRef& operator=(U* object) const requires std::is_base_of_v<U, T>
    {
        m_reference = object;
        return *this;
    }

    /// Allow assignment to the reference from any super type of 'T'.
    template <class U>
    const ObjectPointerRef& operator=(GCRootRef<U> object) const requires std::is_base_of_v<U, T>
    {
        m_reference = object.address();
        return *this;
    }
};

} // namespace jllvm
