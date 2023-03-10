#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/PointerIntPair.h>
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

    /// Returns true if this method is static.
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

/// Class object representing Java 'Class' objects. Class objects serve all introspections needs of Java
/// and additionally serve as the type object of all Java objects. The end of every Class object additionally
/// contains the VTable slots for virtual functions.
class ClassObject final : private llvm::TrailingObjects<ClassObject, VTableSlot>
{
    friend class llvm::TrailingObjects<ClassObject, VTableSlot>;

    std::int32_t m_fieldAreaSize;
    llvm::ArrayRef<Method> m_methods;
    llvm::ArrayRef<Field> m_fields;
    llvm::ArrayRef<const ClassObject*> m_interfaces;
    llvm::StringRef m_className;
    const ClassObject* m_superClass;
    llvm::PointerIntPair<const ClassObject*, 1, bool> m_componentTypeAndIsPrimitive;

    ClassObject(std::int32_t fieldAreaSize, llvm::ArrayRef<Method> methods, llvm::ArrayRef<Field> fields,
                llvm::ArrayRef<const ClassObject*> interfaces, llvm::StringRef className,
                const ClassObject* superClass);

public:
    /// Function to create a new class object for a user class. The class object is allocated within 'allocator'
    /// with 'vTableSlots' amount of v-table slots.
    /// 'fieldAreaSize' is the size of an instance of this class WITHOUT the object header. In other words, this is
    /// only the size of all the fields added up (including of subclasses).
    /// 'methods', 'fields' and 'interfaces' are allocated within 'allocator' to preserve their lifetimes.
    /// 'className' is the name of the user class in the JVM internal format.
    /// 'superClass' is the super class of this class if it has one.
    static ClassObject* create(llvm::BumpPtrAllocator& allocator, std::size_t vTableSlots, std::uint32_t fieldAreaSize,
                               llvm::ArrayRef<Method> methods, llvm::ArrayRef<Field> fields,
                               llvm::ArrayRef<const ClassObject*> interfaces, llvm::StringRef className,
                               const ClassObject* superClass);

    /// Function to create a new class object for an array type. The class object is allocated within 'allocator'
    /// using 'componentType' as the component type of the array type.
    /// 'stringSaver' is used to save the array type descriptor created and used as class name.
    static ClassObject* createArray(llvm::BumpPtrAllocator& allocator, const ClassObject* componentType,
                                    llvm::StringSaver& stringSaver);

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

    /// Returns the direct interfaces implemented by this class.
    llvm::ArrayRef<const ClassObject*> getInterfaces() const
    {
        return m_interfaces;
    }

    /// Returns the name of this class as descriptor.
    llvm::StringRef getClassName() const
    {
        return m_className;
    }

    /// Returns the super class of this class or null if the class does not have a super class.
    /// This is notably the case for array types, primitives and java/lang/Object.
    const ClassObject* getSuperClass() const
    {
        return m_superClass;
    }

    /// Returns true if this class is an array type.
    bool isArray() const
    {
        return getComponentType() != nullptr;
    }

    /// Returns the component type of the array type or null if this is not an array type.
    const ClassObject* getComponentType() const
    {
        return m_componentTypeAndIsPrimitive.getPointer();
    }

    /// Returns true if this is a primitive type.
    bool isPrimitive() const
    {
        return m_componentTypeAndIsPrimitive.getInt();
    }

    /// Returns the VTable slots for the class.
    VTableSlot* getVTable()
    {
        return getTrailingObjects<VTableSlot>();
    }
};

static_assert(std::is_trivially_destructible_v<ClassObject>);
static_assert(std::is_trivially_destructible_v<Field>);
static_assert(std::is_trivially_destructible_v<Method>);

} // namespace jllvm
