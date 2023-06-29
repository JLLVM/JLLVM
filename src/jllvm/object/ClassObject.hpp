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
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/PointerEmbeddedInt.h>
#include <llvm/ADT/PointerIntPair.h>
#include <llvm/ADT/PointerUnion.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/iterator.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/StringSaver.h>
#include <llvm/Support/TrailingObjects.h>

#include <jllvm/class/Descriptors.hpp>

#include "Object.hpp"

namespace jllvm
{
/// Visibility of a method, class or field.
enum class Visibility : std::uint8_t
{
    Package = 0b00,
    Private = 0b01,
    Public = 0b10,
    Protected = 0b11
};

/// Object for representing a classes method.
class Method
{
    llvm::StringRef m_name;
    llvm::StringRef m_type;
    std::uint32_t m_tableSlot;
    std::uint8_t m_hasTableSlot : 1;
    std::uint8_t m_isStatic : 1;
    std::uint8_t m_isFinal : 1;
    std::uint8_t m_isNative : 1;
    std::uint8_t m_visibility : 2;
    std::uint8_t m_isAbstract : 1;

public:
    Method(llvm::StringRef name, llvm::StringRef type, std::optional<std::uint32_t> vTableSlot, bool isStatic,
           bool isFinal, bool isNative, Visibility visibility, bool isAbstract)
        : m_name(name),
          m_type(type),
          m_tableSlot(vTableSlot.value_or(0)),
          m_hasTableSlot(vTableSlot.has_value()),
          m_isStatic(isStatic),
          m_isFinal(isFinal),
          m_isNative(isNative),
          m_visibility(static_cast<std::uint8_t>(visibility)),
          m_isAbstract(isAbstract)
    {
    }

    /// Returns the name of the method.
    llvm::StringRef getName() const
    {
        return m_name;
    }

    /// Returns the JVM descriptor of the method.
    llvm::StringRef getType() const
    {
        return m_type;
    }

    /// Returns either the V-table slot or I-Table (depending on whether the method is part of a class or interface)
    /// of this method if it has one.
    std::optional<std::uint16_t> getTableSlot() const
    {
        if (!m_hasTableSlot)
        {
            return std::nullopt;
        }
        return m_tableSlot;
    }

    /// Returns true if this method is static.
    bool isStatic() const
    {
        return m_isStatic;
    }

    /// Returns true if this method is abstract.
    bool isAbstract() const
    {
        return m_isAbstract;
    }

    /// Returns true if this method is final.
    bool isFinal() const
    {
        return m_isFinal;
    }

    /// Returns true if this method is native.
    bool isNative() const
    {
        return m_isNative;
    }

    Visibility getVisibility() const
    {
        return static_cast<Visibility>(m_visibility);
    }

    /// Returns true if this method is an object constructor.
    bool isObjectConstructor() const
    {
        return getName() == "<init>";
    }
};

/// Object for representing the fields of a class and object.
class Field
{
    llvm::StringRef m_name;
    llvm::StringRef m_type;
    union
    {
        std::uint16_t m_offset;
        char m_primitiveStorage[sizeof(double)];
        void** m_reference;
    };
    bool m_isStatic;

public:
    /// Creates a new non-static field with the given name, type descriptor and its offset within an instance.
    Field(llvm::StringRef name, llvm::StringRef type, std::uint16_t offset)
        : m_name(name), m_type(type), m_offset(offset), m_isStatic(false)
    {
    }

    /// Creates a new static field of a reference type with the given name, type descriptor and a pointer to where the
    /// static reference is allocated.
    Field(llvm::StringRef name, llvm::StringRef type, void** reference)
        : m_name(name), m_type(type), m_reference(reference), m_isStatic(true)
    {
    }

    /// Creates a new static field of a non-reference with the given name, type descriptor.
    Field(llvm::StringRef name, llvm::StringRef type)
        : m_name(name), m_type(type), m_primitiveStorage{}, m_isStatic(true)
    {
    }

    /// Returns the offset of the field within an object.
    /// Calling this method is only valid for non-static fields.
    std::size_t getOffset() const
    {
        assert(!isStatic());
        return m_offset;
    }

    /// Returns the name of this field.
    llvm::StringRef getName() const
    {
        return m_name;
    }

    /// Returns the JVM tpye descriptor of this field.
    llvm::StringRef getType() const
    {
        return m_type;
    }

    /// Returns true if this field is static.
    bool isStatic() const
    {
        return m_isStatic;
    }

    /// Returns the address to the storage of this static variable.
    /// This points either within the static-reference heap in 'GarbageCollector' if it is a reference type or
    /// to inline storage within 'Field' if it is a primitive type.
    /// Calling this method is invalid for non-static objects.
    const void* getAddressOfStatic() const
    {
        assert(isStatic());
        if (jllvm::isReferenceDescriptor(m_type))
        {
            return m_reference;
        }
        return reinterpret_cast<const void*>(m_primitiveStorage);
    }
};

using VTableSlot = void*;

/// Class used for storing a 'ClassObject's implementations of interface methods of the interface with a given id.
/// This is a variable length object where the function pointers start at the end of the object.
class ITable final : public llvm::TrailingObjects<ITable, VTableSlot>
{
    friend class llvm::TrailingObjects<ITable, VTableSlot>;

    std::size_t m_id;

    explicit ITable(std::size_t id) : m_id(id) {}

public:
    /// Creates a new ITable by allocating it in 'allocator' with the given ID and enough storage of 'iTableSlots'
    /// amount of methods.
    static ITable* create(llvm::BumpPtrAllocator& allocator, std::size_t id, std::size_t iTableSlots);

    /// Returns the ID of the interface this ITable corresponds to.
    std::size_t getId() const
    {
        return m_id;
    }

    /// Returns a pointer to the first slot in the ITable. Slots are contiguous and therefore reachable by pointer
    /// arithmetic.
    VTableSlot* getMethods()
    {
        return getTrailingObjects<VTableSlot>();
    }
};

/// Class object representing Java 'Class' objects. Class objects serve all introspections needs of Java
/// and additionally serve as the type object of all Java objects. The end of every Class object additionally
/// contains the VTable slots for virtual functions.
class ClassObject final : private llvm::TrailingObjects<ClassObject, VTableSlot>, public ObjectInterface
{
    friend class llvm::TrailingObjects<ClassObject, VTableSlot>;
    ObjectHeader m_objectHeader;

    using InterfaceId = llvm::PointerEmbeddedInt<std::size_t, std::numeric_limits<std::size_t>::digits - 1>;

    // Field layout from Java!
    Object* m_cachedConstructor = nullptr;
    // This is purely used as a cache by the JVM and lazily init.
    String* m_name = nullptr;
    Object* m_module = nullptr;
    Object* m_classLoader = nullptr;
    Object* m_classData = nullptr;
    String* m_packageName = nullptr;
    llvm::PointerUnion<const ClassObject*, InterfaceId> m_componentTypeOrInterfaceId;
    Object* m_reflectionData = nullptr;
    std::int32_t m_classRedefinedCount = 0;
    Object* m_genericInfo = nullptr;
    Array<Object*>* m_enumConstants = nullptr;
    Object* m_enumConstantDirectory = nullptr;
    Object* m_annotationData = nullptr;
    Object* m_annotationType = nullptr;
    Object* m_classValueMap = nullptr;

    // Custom data we add starts here. Since ClassObjects are always created in the class loader heap and never
    // directly form Java code or on the GC we can extend the layout given by the JDK.
    std::int32_t m_fieldAreaSize;
    // V-Table size for classes, I-Table size for Interfaces.
    std::int32_t m_tableSize;
    llvm::ArrayRef<Method> m_methods;
    llvm::ArrayRef<Field> m_fields;
    // Contains all the bases of this class object. For classes, this contains the superclass (except for 'Object')
    // followed by all direct superinterfaces. For interfaces, this is simply their direct superinterfaces.
    llvm::ArrayRef<ClassObject*> m_bases;
    llvm::ArrayRef<ITable*> m_iTables;
    llvm::ArrayRef<std::uint32_t> m_gcMask;
    llvm::StringRef m_className;
    bool m_isPrimitive = false;
    bool m_initialized = false;
    bool m_isAbstract = false;

    ClassObject(const ClassObject* metaClass, std::uint32_t vTableSlots, std::int32_t fieldAreaSize,
                llvm::ArrayRef<Method> methods, llvm::ArrayRef<Field> fields, llvm::ArrayRef<ClassObject*> bases,
                llvm::ArrayRef<ITable*> iTables, llvm::StringRef className, bool isAbstract,
                llvm::ArrayRef<std::uint32_t> gcMask);

    ClassObject(const ClassObject* metaClass, std::size_t interfaceId, llvm::ArrayRef<Method> methods,
                llvm::ArrayRef<Field> fields, llvm::ArrayRef<ClassObject*> interfaces, llvm::StringRef className);

    class SuperclassIterator
        : public llvm::iterator_facade_base<SuperclassIterator, std::forward_iterator_tag, const ClassObject*,
                                            std::ptrdiff_t, const ClassObject**, const ClassObject*>
    {
        const ClassObject* m_curr = nullptr;

    public:
        SuperclassIterator() = default;

        explicit SuperclassIterator(const ClassObject* curr) : m_curr(curr) {}

        bool operator==(const SuperclassIterator& rhs) const
        {
            return m_curr == rhs.m_curr;
        }

        const ClassObject* operator*() const
        {
            return m_curr;
        }

        SuperclassIterator& operator++()
        {
            m_curr = m_curr->getSuperClass();
            return *this;
        }
    };

public:
    /// Function to create a new class object for a user class. The class object is allocated within 'allocator'
    /// with 'vTableSlots' amount of v-table slots.
    /// 'fieldAreaSize' is the size of an instance of this class WITHOUT the object header. In other words, this is
    /// only the size of all the fields added up (including of subclasses).
    /// 'methods', 'fields' and 'bases' are allocated within 'allocator' to preserve their lifetimes.
    /// 'bases' must contain all direct superclasses and interfaces implemented by the class object, with the
    /// superclass in the very first position (if it has one).
    /// 'className' is the name of the user class in the JVM internal format.
    static ClassObject* create(llvm::BumpPtrAllocator& allocator, const ClassObject* metaClass,
                               std::uint32_t vTableSlots, std::uint32_t fieldAreaSize, llvm::ArrayRef<Method> methods,
                               llvm::ArrayRef<Field> fields, llvm::ArrayRef<ClassObject*> bases,
                               llvm::StringRef className, bool isAbstract);

    /// Function to create a new class object for an interface. The class object is allocated within 'allocator'.
    /// 'interfaceId' is the globally unique id of this interface.
    /// 'methods', 'fields' and 'interfaces' are allocated within 'allocator' to preserve their lifetimes.
    /// 'className' is the name of the user class in the JVM internal format.
    static ClassObject* createInterface(llvm::BumpPtrAllocator& allocator, const ClassObject* metaClass,
                                        std::size_t interfaceId, llvm::ArrayRef<Method> methods,
                                        llvm::ArrayRef<Field> fields, llvm::ArrayRef<ClassObject*> interfaces,
                                        llvm::StringRef className);

    /// Function to create a new class object for an array type. The class object is allocated within 'allocator'
    /// using 'componentType' as the component type of the array type.
    /// 'stringSaver' is used to save the array type descriptor created and used as class name.
    static ClassObject* createArray(llvm::BumpPtrAllocator& allocator, const ClassObject* metaClass,
                                    const ClassObject* componentType, llvm::StringSaver& stringSaver);

    /// Constructor for creating the class objects for primitive types with a size and name.
    ClassObject(std::uint32_t instanceSize, llvm::StringRef name);

    /// Byte offset from the start of the class object to the field area size member.
    constexpr static std::size_t getFieldAreaSizeOffset()
    {
        return offsetof(ClassObject, m_fieldAreaSize);
    }

    /// Size of an instance of this class object WITHOUT the object header.
    /// This essentially means it is the size of all fields, including any super classes, added up.
    /// For arrays this returns the size of the 'length' field and potentially the size of the padding between the
    /// length field and the elements after. It does NOT include the elements themselves.
    /// Note: This is invalid for primitives.
    std::uint32_t getFieldAreaSize() const
    {
        assert(!isPrimitive());
        return m_fieldAreaSize;
    }

    /// Size of an instance of this class, returning ALL bytes composing of the type including the object header
    /// IF the object is not an array object. For array objects it does NOT contain the array elements, but does contain
    /// the length field with any padding afterwards *if and only if required*.
    std::uint32_t getInstanceSize() const
    {
        return m_fieldAreaSize + sizeof(ObjectHeader);
    }

    /// Returns the GC mask for instances of this class object.
    /// The GC mask is an ordered array of indices, which if multiplied by the pointer size, create offsets from
    /// **after** the object header, into a field of reference type within an instance.
    llvm::ArrayRef<std::uint32_t> getGCObjectMask() const
    {
        return m_gcMask;
    }

    /// Returns the methods of this class.
    llvm::ArrayRef<Method> getMethods() const
    {
        return m_methods;
    }

    /// Returns the fields of this class.
    llvm::ArrayRef<Field> getFields() const
    {
        return m_fields;
    }

    /// Returns the field with the given 'fieldName' and 'fieldType', only considering static or instance fields
    /// depending on 'isStatic'.
    /// This field lookup unlike 'getFields' also considers fields in the base classes of this class.
    /// Returns nullptr if no field was found.
    const Field* getField(llvm::StringRef fieldName, llvm::StringRef fieldType, bool isStatic) const;

    /// Returns all direct superclasses and superinterfaces of the class object.
    llvm::ArrayRef<ClassObject*> getBases() const
    {
        return m_bases;
    }

    /// Returns the direct interfaces implemented by this class.
    llvm::ArrayRef<const ClassObject*> getInterfaces() const
    {
        return m_bases.drop_front(isClass() ? 1 : 0);
    }

    /// Returns a range containing all direct and indirect interfaces of this class in an unspecified order.
    auto getAllInterfaces() const;

    /// Returns the name of this class as descriptor.
    llvm::StringRef getClassName() const
    {
        return m_className;
    }

    /// Returns the name of the package this class is defined in.
    /// Note: This is not properly checked whether that is how the JVM spec actually defines, but we currently define
    /// it as the part before the last '/'.
    llvm::StringRef getPackageName() const
    {
        auto [package, clazz] = m_className.rsplit('/');
        if (clazz.empty())
        {
            // If there is no /, 'clazz' is empty and 'package' is actually the class name.
            // We assume the package to be the empty string for now.
            return clazz;
        }
        return package;
    }

    /// Returns the super class of this class or null if the class does not have a super class.
    /// This is notably the case for array types, primitives and java/lang/Object.
    const ClassObject* getSuperClass() const
    {
        return (m_bases.empty() || !m_bases.front()->isClass()) ? nullptr : m_bases.front();
    }

    /// Returns a range of all super classes of this class object, by default starting with this class object.
    auto getSuperClasses(bool includeThis = true) const
    {
        return llvm::drop_begin(llvm::make_range(SuperclassIterator(this), SuperclassIterator()), includeThis ? 0 : 1);
    }

    /// Returns true if this class is an array type.
    bool isArray() const
    {
        return getComponentType() != nullptr;
    }

    /// Returns true if this class is an interface.
    bool isInterface() const
    {
        return m_componentTypeOrInterfaceId.is<InterfaceId>();
    }

    /// Returns the globally unique interface id of this interface.
    std::size_t getInterfaceId() const
    {
        return m_componentTypeOrInterfaceId.get<InterfaceId>();
    }

    /// Returns the component type of the array type or null if this is not an array type.
    const ClassObject* getComponentType() const
    {
        return m_componentTypeOrInterfaceId.dyn_cast<const ClassObject*>();
    }

    /// Returns true if this is a primitive type.
    bool isPrimitive() const
    {
        return m_isPrimitive;
    }

    /// Returns true if this class object represents a Java class.
    /// This does not include array types or interfaces.
    bool isClass() const
    {
        return !isArray() && !isPrimitive() && !isInterface();
    }

    /// Returns true if this class object represents an abstract Java class.
    bool isAbstract() const
    {
        return m_isAbstract;
    }

    /// Returns true if an instance of this class object would also be an instance of 'other'.
    /// Not valid for class objects representing interfaces.
    bool wouldBeInstanceOf(const ClassObject* other) const;

    /// Byte offset from the start of the class object to the start of the VTable.
    constexpr static std::size_t getInitializedOffset()
    {
        return offsetof(ClassObject, m_initialized);
    }

    bool isInitialized() const
    {
        return m_initialized;
    }

    void setInitialized(bool isInitialized)
    {
        m_initialized = isInitialized;
    }

    /// Byte offset from the start of the class object to the start of the VTable.
    constexpr static std::size_t getVTableOffset()
    {
        return sizeof(ClassObject);
    }

    /// Returns the VTable slots for the class.
    llvm::MutableArrayRef<VTableSlot> getVTable()
    {
        return {getTrailingObjects<VTableSlot>(), isAbstract() || isInterface() ? 0 : getTableSize()};
    }

    /// Returns the list of ITables of this class.
    llvm::ArrayRef<ITable*> getITables() const
    {
        return m_iTables;
    }

    /// Byte offset from the start of the class object to the field area size member.
    constexpr static std::size_t getITablesOffset()
    {
        return offsetof(ClassObject, m_iTables);
    }

    /// Returns the size of the I-Table if this class object represents an interface and the size of the V-Table
    /// otherwise.
    /// If the class is abstract, the v-table size does not reflect the actual size of the v-table of this class, as
    /// abstract classes do not have any, but rather the v-table size any subclasses need to accommodate when
    /// inheriting from this class.
    std::uint32_t getTableSize() const
    {
        return m_tableSize;
    }

    /// Returns a range of all (direct and indirect) interfaces of this class object in order of "maximally specific" as
    /// the JVM spec calls it.
    /// This is simply all interfaces of this class in topological sort order of the interface inheritance DAG.
    /// All subinterfaces are therefore guaranteed to appear before their base interface in this list.
    /// Note: This is an expensive operation and should only be used if a topological traversal order is required.
    auto maximallySpecificInterfaces() const;
};

static_assert(std::is_trivially_destructible_v<ClassObject>);
static_assert(std::is_trivially_destructible_v<Field>);
static_assert(std::is_trivially_destructible_v<Method>);
static_assert(std::is_standard_layout_v<ClassObject>);

/// Adaptor class for LLVMs GraphTraits used to indicate we want to traverse the class object graph of a 'ClassObject'.
struct ClassGraph
{
    const ClassObject* root;
};

} // namespace jllvm

template <>
struct llvm::GraphTraits<jllvm::ClassGraph>
{
    using NodeRef = const jllvm::ClassObject*;
    using ChildIteratorType = llvm::ArrayRef<const jllvm::ClassObject*>::iterator;

    static NodeRef getEntryNode(jllvm::ClassGraph classGraph)
    {
        return classGraph.root;
    }

    static ChildIteratorType child_begin(NodeRef classObject)
    {
        return classObject->getBases().begin();
    }

    static ChildIteratorType child_end(NodeRef classObject)
    {
        return classObject->getBases().end();
    }
};

inline auto jllvm::ClassObject::getAllInterfaces() const
{
    // Range class which combines a depth first walk over the class graph with a filter for interface classes.
    // The reason we can't compose this out of other ranges is because there is a bug in LLVMs 'llvm::depth_first' and
    // other graph traversal iterators causing compilation errors with the filter class.
    // See https://reviews.llvm.org/D151198
    class OwningFilterRange
    {
        llvm::df_iterator_default_set<llvm::GraphTraits<ClassGraph>::NodeRef> m_set;
        llvm::iterator_range<llvm::df_ext_iterator<ClassGraph>> m_range;

    public:
        explicit OwningFilterRange(const ClassObject* object)
            : m_range(llvm::depth_first_ext(ClassGraph{object}, m_set))
        {
        }

        class iterator : public llvm::iterator_facade_base<iterator, std::input_iterator_tag, const ClassObject*,
                                                           std::ptrdiff_t, const ClassObject**, const ClassObject*>
        {
            llvm::df_ext_iterator<ClassGraph> m_current;
            llvm::df_ext_iterator<ClassGraph> m_end;

        public:
            explicit iterator(const llvm::df_ext_iterator<ClassGraph>& begin,
                              const llvm::df_ext_iterator<ClassGraph>& end)
                : m_current(std::find_if(begin, end, std::mem_fn(&ClassObject::isInterface))), m_end(end)
            {
            }

            bool operator==(const iterator& rhs) const
            {
                return m_current == rhs.m_current;
            }

            const ClassObject* operator*() const
            {
                return *m_current;
            }

            iterator& operator++()
            {
                do
                {
                    ++m_current;
                } while (m_current != m_end && !m_current->isInterface());
                return *this;
            }
        };

        auto begin() const
        {
            return iterator(m_range.begin(), m_range.end());
        }

        auto end() const
        {
            return iterator(m_range.end(), m_range.end());
        }
    };

    return OwningFilterRange(this);
}

inline auto jllvm::ClassObject::maximallySpecificInterfaces() const
{
    // Adaptor for ReversePostOrderTraversal which keeps the range alive and filters non-classes.
    class RPODropFront
    {
        llvm::ReversePostOrderTraversal<ClassGraph> m_traversal;

    public:
        explicit RPODropFront(const ClassObject* object) : m_traversal(ClassGraph{object}) {}

        auto begin() const
        {
            return llvm::filter_iterator<decltype(m_traversal)::const_rpo_iterator,
                                         decltype(std::mem_fn(&ClassObject::isInterface))>(
                m_traversal.begin(), m_traversal.end(), std::mem_fn(&ClassObject::isInterface));
        }

        auto end() const
        {
            return llvm::filter_iterator<decltype(m_traversal)::const_rpo_iterator,
                                         decltype(std::mem_fn(&ClassObject::isInterface))>(
                m_traversal.end(), m_traversal.end(), std::mem_fn(&ClassObject::isInterface));
        }
    };

    return RPODropFront{this};
}
