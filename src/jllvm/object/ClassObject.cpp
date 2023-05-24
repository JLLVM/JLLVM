#include "ClassObject.hpp"

#include <llvm/ADT/Twine.h>

namespace
{
template <class Range>
auto arrayRefAlloc(llvm::BumpPtrAllocator& allocator, const Range& ref)
{
    auto* storage = allocator.Allocate<typename Range::value_type>(ref.size());
    llvm::copy(ref, storage);
    return llvm::ArrayRef{storage, ref.size()};
}
} // namespace

jllvm::ClassObject* jllvm::ClassObject::create(llvm::BumpPtrAllocator& allocator, const ClassObject* metaClass,
                                               std::size_t vTableSlots, std::uint32_t fieldAreaSize,
                                               llvm::ArrayRef<Method> methods, llvm::ArrayRef<Field> fields,
                                               llvm::ArrayRef<const ClassObject*> bases, llvm::StringRef className)
{
    llvm::SmallVector<ITable*> iTables;
    llvm::df_iterator_default_set<const jllvm::ClassObject*> seen;
    for (const ClassObject* root : bases)
    {
        for (const ClassObject* classObject : llvm::depth_first_ext(ClassGraph{root}, seen))
        {
            if (!classObject->isInterface())
            {
                continue;
            }
            // TODO: We should probably calculate the method count of an interface once and store it in the interface.
            iTables.push_back(ITable::create(
                allocator, classObject->getInterfaceId(),
                llvm::count_if(classObject->getMethods(), [](const Method& method) { return !method.isStatic(); })));
        }
    }

    auto* result =
        new (allocator.Allocate(ClassObject::totalSizeToAlloc<VTableSlot>(vTableSlots), alignof(ClassObject)))
            ClassObject(metaClass, fieldAreaSize, arrayRefAlloc(allocator, methods), arrayRefAlloc(allocator, fields),
                        arrayRefAlloc(allocator, bases), arrayRefAlloc(allocator, iTables), className);
    // Zero out vTable.
    std::memset(result->getVTable(), 0, vTableSlots * sizeof(VTableSlot));
    return result;
}

jllvm::ClassObject::ClassObject(const ClassObject* metaClass, std::int32_t fieldAreaSize,
                                llvm::ArrayRef<Method> methods, llvm::ArrayRef<Field> fields,
                                llvm::ArrayRef<const ClassObject*> bases, llvm::ArrayRef<ITable*> iTables,
                                llvm::StringRef className)
    : m_objectHeader(metaClass),
      m_fieldAreaSize(fieldAreaSize),
      m_methods(methods),
      m_fields(fields),
      m_bases(bases),
      m_iTables(iTables),
      m_className(className)
{
}

jllvm::ClassObject* jllvm::ClassObject::createArray(llvm::BumpPtrAllocator& allocator, const ClassObject* metaClass,
                                                    const ClassObject* componentType, llvm::StringSaver& stringSaver)
{
    std::uint32_t arrayFieldAreaSize = sizeof(std::uint32_t);
    // Account for padding inbetween 'length' and the elements after.
    if (componentType->isPrimitive())
    {
        arrayFieldAreaSize = llvm::alignTo(arrayFieldAreaSize, componentType->getInstanceSize());
    }
    else
    {
        arrayFieldAreaSize = llvm::alignTo(arrayFieldAreaSize, sizeof(void*));
    }

    auto* result = create(allocator, metaClass, 0, arrayFieldAreaSize, {}, {}, {},
                          stringSaver.save("[L" + componentType->getClassName() + ";"));
    result->m_componentTypeOrInterfaceId = componentType;
    return result;
}

jllvm::ClassObject::ClassObject(std::uint32_t instanceSize, llvm::StringRef name)
    : ClassObject(nullptr, instanceSize - sizeof(ObjectHeader), {}, {}, {}, {}, name)
{
    m_isPrimitive = true; // NOLINT(*-prefer-member-initializer): https://github.com/llvm/llvm-project/issues/52818
}

const jllvm::Field* jllvm::ClassObject::getField(llvm::StringRef fieldName, llvm::StringRef fieldType,
                                                 bool isStatic) const
{
    for (const ClassObject* curr : getSuperClasses())
    {
        const Field* iter = llvm::find_if(
            curr->getFields(), [&](const Field& field)
            { return field.isStatic() == isStatic && field.getName() == fieldName && field.getType() == fieldType; });
        if (iter != curr->getFields().end())
        {
            return iter;
        }
    }
    return nullptr;
}

jllvm::ClassObject::ClassObject(const ClassObject* metaClass, std::size_t interfaceId, llvm::ArrayRef<Method> methods,
                                llvm::ArrayRef<Field> fields, llvm::ArrayRef<const ClassObject*> interfaces,
                                llvm::StringRef className)
    : m_objectHeader(metaClass),
      m_fieldAreaSize(0),
      m_methods(methods),
      m_fields(fields),
      m_bases(interfaces),
      m_className(className),
      m_componentTypeOrInterfaceId(interfaceId)
{
}

jllvm::ClassObject* jllvm::ClassObject::createInterface(llvm::BumpPtrAllocator& allocator, const ClassObject* metaClass,
                                                        std::size_t interfaceId, llvm::ArrayRef<Method> methods,
                                                        llvm::ArrayRef<Field> fields,
                                                        llvm::ArrayRef<const ClassObject*> interfaces,
                                                        llvm::StringRef className)
{
    return new (allocator.Allocate<ClassObject>())
        ClassObject(metaClass, interfaceId, arrayRefAlloc(allocator, methods), arrayRefAlloc(allocator, fields),
                    arrayRefAlloc(allocator, interfaces), className);
}

jllvm::ITable* jllvm::ITable::create(llvm::BumpPtrAllocator& allocator, std::size_t id, std::size_t iTableSlots)
{
    auto* result =
        new (allocator.Allocate(ITable::totalSizeToAlloc<VTableSlot>(iTableSlots), alignof(ITable))) ITable(id);
    std::memset(result->getTrailingObjects<VTableSlot>(), 0, sizeof(VTableSlot) * iTableSlots);
    return result;
}

bool jllvm::ClassObject::wouldBeInstanceOf(const ClassObject* other) const
{
    assert(!isInterface());
    if (this == other)
    {
        return true;
    }

    // Primitive class objects have no concept of inheritance.
    if (isPrimitive() || other->isPrimitive())
    {
        return false;
    }

    if (isArray())
    {
        if (!other->isArray())
        {
            // classObject has to be Object if not an array type.
            // Object is easy to identify as it is a normal class with no super class.
            return other->isClass() && !other->getSuperClass();
        }

        // Strip array types and check that the component types are compatible.
        const ClassObject* curr = this;
        while (curr->isArray() && other->isArray())
        {
            curr = curr->getComponentType();
            other = other->getComponentType();
        }
        if (curr->isArray() || other->isArray())
        {
            // Not the same depth of array types.
            return false;
        }
        return curr->wouldBeInstanceOf(other);
    }

    if (other->isInterface())
    {
        return llvm::is_contained(getAllInterfaces(), other);
    }
    return llvm::is_contained(getSuperClasses(), other);
}
