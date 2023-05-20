#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/iterator.h>
#include <llvm/ADT/PointerEmbeddedInt.h>
#include <llvm/ADT/PointerIntPair.h>
#include <llvm/ADT/PointerUnion.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/StringRef.h>
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
    std::uint16_t m_vTableSlot;
    std::uint8_t m_hasVTableSlot : 1;
    std::uint8_t m_isStatic : 1;
    std::uint8_t m_isFinal : 1;
    std::uint8_t m_isNative : 1;
    std::uint8_t m_visibility : 2;
    std::uint8_t m_isAbstract : 1;

public:
    Method(llvm::StringRef name, llvm::StringRef type, std::optional<std::uint16_t> vTableSlot, bool isStatic,
           bool isFinal, bool isNative, Visibility visibility, bool isAbstract)
        : m_name(name),
          m_type(type),
          m_vTableSlot(vTableSlot.value_or(0)),
          m_hasVTableSlot(vTableSlot.has_value()),
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

    /// Returns the v-table slot of this method if it has one.
    /// Note that a method may or may-not have a v-table slot regardless of if the method can be overwritten.
    std::optional<std::uint16_t> getVTableSlot() const
    {
        if (!m_hasVTableSlot)
        {
            return std::nullopt;
        }
        return m_vTableSlot;
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

    // Field layout from Java!
    Object* m_cachedConstructor = nullptr;
    // This is purely used as a cache by the JVM and lazily init.
    String* m_name = nullptr;
    Object* m_module = nullptr;
    Object* m_classLoader = nullptr;
    Object* m_classData = nullptr;
    String* m_packageName = nullptr;
    const ClassObject* m_componentType = nullptr;
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
    llvm::ArrayRef<Method> m_methods;
    llvm::ArrayRef<Field> m_fields;
    llvm::ArrayRef<const ClassObject*> m_interfaces;
    llvm::ArrayRef<ITable*> m_iTables;
    llvm::StringRef m_className;

    using InterfaceId = llvm::PointerEmbeddedInt<std::size_t, std::numeric_limits<std::size_t>::digits - 1>;

    llvm::PointerUnion<const ClassObject*, InterfaceId> m_superClassOrInterfaceId;
    bool m_isPrimitive = false;

    ClassObject(const ClassObject* metaClass, std::int32_t fieldAreaSize, llvm::ArrayRef<Method> methods,
                llvm::ArrayRef<Field> fields, llvm::ArrayRef<const ClassObject*> interfaces,
                llvm::ArrayRef<ITable*> iTables, llvm::StringRef className, const ClassObject* superClass);

    ClassObject(const ClassObject* metaClass, std::size_t interfaceId, llvm::ArrayRef<Method> methods,
                llvm::ArrayRef<Field> fields, llvm::ArrayRef<const ClassObject*> interfaces, llvm::StringRef className);

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
    /// 'methods', 'fields' and 'interfaces' are allocated within 'allocator' to preserve their lifetimes.
    /// 'className' is the name of the user class in the JVM internal format.
    /// 'superClass' is the super class of this class if it has one.
    static ClassObject* create(llvm::BumpPtrAllocator& allocator, const ClassObject* metaClass, std::size_t vTableSlots,
                               std::uint32_t fieldAreaSize, llvm::ArrayRef<Method> methods,
                               llvm::ArrayRef<Field> fields, llvm::ArrayRef<const ClassObject*> interfaces,
                               llvm::StringRef className, const ClassObject* superClass);

    /// Function to create a new class object for an interface. The class object is allocated within 'allocator'.
    /// 'interfaceId' is the globally unique id of this interface.
    /// 'methods', 'fields' and 'interfaces' are allocated within 'allocator' to preserve their lifetimes.
    /// 'className' is the name of the user class in the JVM internal format.
    static ClassObject* createInterface(llvm::BumpPtrAllocator& allocator, const ClassObject* metaClass,
                                        std::size_t interfaceId, llvm::ArrayRef<Method> methods,
                                        llvm::ArrayRef<Field> fields, llvm::ArrayRef<const ClassObject*> interfaces, llvm::StringRef className);

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
    /// the length field.
    std::uint32_t getInstanceSize() const
    {
        return m_fieldAreaSize + sizeof(ObjectHeader);
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

    /// Returns the direct interfaces implemented by this class.
    llvm::ArrayRef<const ClassObject*> getInterfaces() const
    {
        return m_interfaces;
    }

    /// Returns a range containing all direct and indirect interfaces of this class in an unspecified order.
    auto getAllInterfaces() const;

    /// Returns the name of this class as descriptor.
    llvm::StringRef getClassName() const
    {
        return m_className;
    }

    /// Returns the super class of this class or null if the class does not have a super class.
    /// This is notably the case for array types, primitives and java/lang/Object.
    const ClassObject* getSuperClass() const
    {
        return m_superClassOrInterfaceId.dyn_cast<const ClassObject*>();
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
        return m_superClassOrInterfaceId.is<InterfaceId>();
    }

    /// Returns the globally unique interface id of this interface.
    std::size_t getInterfaceId() const
    {
        return m_superClassOrInterfaceId.get<InterfaceId>();
    }

    /// Returns the component type of the array type or null if this is not an array type.
    const ClassObject* getComponentType() const
    {
        return m_componentType;
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

    /// Returns true if an instance of this class object would also be an instance of 'other'.
    /// Not valid for class objects representing interfaces.
    bool wouldBeInstanceOf(const ClassObject* other) const;

    /// Byte offset from the start of the class object to the start of the VTable.
    constexpr static std::size_t getVTableOffset()
    {
        return sizeof(ClassObject);
    }

    /// Returns the VTable slots for the class.
    VTableSlot* getVTable()
    {
        return getTrailingObjects<VTableSlot>();
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

/// Adaptor class for LLVMs GraphTraits used to indicate we want to traverse the interfaces of a 'ClassObject'.
struct InterfaceGraph
{
    const ClassObject* root;
};

} // namespace jllvm

template <>
struct llvm::GraphTraits<jllvm::InterfaceGraph>
{
    using NodeRef = const jllvm::ClassObject*;
    using ChildIteratorType = llvm::ArrayRef<const jllvm::ClassObject*>::iterator;

    static NodeRef getEntryNode(jllvm::InterfaceGraph interfaceGraph)
    {
        return interfaceGraph.root;
    }

    static ChildIteratorType child_begin(NodeRef interface)
    {
        return interface->getInterfaces().begin();
    }

    static ChildIteratorType child_end(NodeRef interface)
    {
        return interface->getInterfaces().end();
    }
};

inline auto jllvm::ClassObject::getAllInterfaces() const
{
    return llvm::drop_begin(llvm::depth_first(InterfaceGraph{this}));
}

inline auto jllvm::ClassObject::maximallySpecificInterfaces() const
{
    // Adaptor for ReversePostOrderTraversal which drops this first element (as that is 'this').
    // llvm::drop_begin doesn't work here since that does not keep the range passed to it alive, just the iterators.
    class RPODropFront
    {
        llvm::ReversePostOrderTraversal<InterfaceGraph> m_traversal;

    public:
        explicit RPODropFront(const ClassObject* object) : m_traversal(InterfaceGraph{object}) {}

        auto begin() const
        {
            return std::next(m_traversal.begin());
        }

        auto end() const
        {
            return m_traversal.end();
        }
    };

    return RPODropFront{this};
}
