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
                                               std::uint32_t vTableSlots, std::uint32_t fieldAreaSize,
                                               llvm::ArrayRef<Method> methods, llvm::ArrayRef<Field> fields,
                                               llvm::ArrayRef<ClassObject*> bases, llvm::StringRef className,
                                               bool isAbstract)
{
    llvm::SmallVector<ITable*> iTables;
    if (!isAbstract)
    {
        llvm::df_iterator_default_set<const jllvm::ClassObject*> seen;
        for (const ClassObject* root : bases)
        {
            for (const ClassObject* classObject : llvm::depth_first_ext(ClassGraph{root}, seen))
            {
                if (!classObject->isInterface())
                {
                    continue;
                }
                iTables.push_back(
                    ITable::create(allocator, classObject->getInterfaceId(), classObject->getTableSize()));
            }
        }
    }

    std::vector<std::uint32_t> gcMask;
    if (!bases.empty() && bases.front()->isClass())
    {
        gcMask = bases.front()->getGCObjectMask();
    }

    for (const Field& iter : fields)
    {
        if (iter.isStatic() || !isReferenceDescriptor(iter.getType()))
        {
            continue;
        }
        // Reference fields are always pointer aligned, making the division here lossless.
        gcMask.emplace_back(iter.getOffset() / sizeof(Object*));
    }

    // Abstract classes don't need a V-Table since they can't be instantiated and therefore can't ever occur as
    // class object in an 'invokevirtual' instruction.
    // Their methods nevertheless have v-table slot assignments since subclasses can call them and need to account for
    // them in their v-table size.
    std::uint32_t allocatedVTableSlots = isAbstract ? 0 : vTableSlots;
    void* storage =
        allocator.Allocate(ClassObject::totalSizeToAlloc<VTableSlot>(allocatedVTableSlots), alignof(ClassObject));
    auto* result = new (storage)
        ClassObject(metaClass, vTableSlots, fieldAreaSize, arrayRefAlloc(allocator, methods),
                    arrayRefAlloc(allocator, fields), arrayRefAlloc(allocator, bases),
                    arrayRefAlloc(allocator, iTables), className, isAbstract, arrayRefAlloc(allocator, gcMask));
    // Zero out vTable.
    std::fill(result->getVTable().begin(), result->getVTable().end(), nullptr);
    return result;
}

jllvm::ClassObject::ClassObject(const ClassObject* metaClass, std::uint32_t vTableSlots, std::int32_t fieldAreaSize,
                                llvm::ArrayRef<Method> methods, llvm::ArrayRef<Field> fields,
                                llvm::ArrayRef<ClassObject*> bases, llvm::ArrayRef<ITable*> iTables,
                                llvm::StringRef className, bool isAbstract, llvm::ArrayRef<std::uint32_t> gcMask)
    : m_objectHeader(metaClass),
      m_fieldAreaSize(fieldAreaSize),
      m_tableSize(vTableSlots),
      m_methods(methods),
      m_fields(fields),
      m_bases(bases),
      m_iTables(iTables),
      m_className(className),
      m_gcMask(gcMask),
      m_isAbstract(isAbstract)
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

    llvm::StringRef className = componentType->getClassName();
    auto* result =
        create(allocator, metaClass, 0, arrayFieldAreaSize, {}, {}, {},
               stringSaver.save(componentType->isClass() || componentType->isInterface() ? "[L" + className + ";" :
                                                                                           "[" + className),
               false);
    result->m_componentTypeOrInterfaceId = componentType;
    result->m_initialized = true;
    return result;
}

jllvm::ClassObject::ClassObject(std::uint32_t instanceSize, llvm::StringRef name)
    : ClassObject(nullptr, 0, instanceSize - sizeof(ObjectHeader), {}, {}, {}, {}, name, false, {})
{
    // NOLINTBEGIN(*-prefer-member-initializer): https://github.com/llvm/llvm-project/issues/52818
    m_isPrimitive = true;
    m_initialized = true;
    // NOLINTEND(*-prefer-member-initializer)
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
                                llvm::ArrayRef<Field> fields, llvm::ArrayRef<ClassObject*> interfaces,
                                llvm::StringRef className)
    : m_objectHeader(metaClass),
      m_fieldAreaSize(0),
      m_tableSize(llvm::count_if(methods, [](const Method& method) { return method.getTableSlot(); })),
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
                                                        llvm::ArrayRef<ClassObject*> interfaces,
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
