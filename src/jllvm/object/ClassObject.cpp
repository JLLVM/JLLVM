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

jllvm::ClassObject* jllvm::ClassObject::create(llvm::BumpPtrAllocator& allocator, std::size_t vTableSlots,
                                               std::uint32_t fieldAreaSize, llvm::ArrayRef<Method> methods,
                                               llvm::ArrayRef<Field> fields,
                                               llvm::ArrayRef<const ClassObject*> interfaces, llvm::StringRef className,
                                               const jllvm::ClassObject* superClass)
{
    llvm::SmallVector<ITable*> iTables;
    llvm::df_iterator_default_set<const jllvm::ClassObject*> seen;
    for (const ClassObject* root : interfaces)
    {
        for (const ClassObject* interface : llvm::depth_first_ext(InterfaceGraph{root}, seen))
        {
            // TODO: We should probably calculate the method count of an interface once and store it in the interface.
            iTables.push_back(ITable::create(
                allocator, interface->getInterfaceId(),
                llvm::count_if(interface->getMethods(), [](const Method& method) { return !method.isStatic(); })));
        }
    }

    auto* result =
        new (allocator.Allocate(ClassObject::totalSizeToAlloc<VTableSlot>(vTableSlots), alignof(ClassObject)))
            ClassObject(fieldAreaSize, arrayRefAlloc(allocator, methods), arrayRefAlloc(allocator, fields),
                        arrayRefAlloc(allocator, interfaces), arrayRefAlloc(allocator, iTables), className, superClass);
    // Zero out vTable.
    std::memset(result->getVTable(), 0, vTableSlots * sizeof(VTableSlot));
    return result;
}

jllvm::ClassObject::ClassObject(std::int32_t fieldAreaSize, llvm::ArrayRef<Method> methods,
                                llvm::ArrayRef<Field> fields, llvm::ArrayRef<const ClassObject*> interfaces,
                                llvm::ArrayRef<ITable*> iTables, llvm::StringRef className,
                                const jllvm::ClassObject* superClass)
    : m_fieldAreaSize(fieldAreaSize),
      m_methods(methods),
      m_fields(fields),
      m_interfaces(interfaces),
      m_iTables(iTables),
      m_className(className),
      m_superClassOrInterfaceId(superClass)
{
}

jllvm::ClassObject* jllvm::ClassObject::createArray(llvm::BumpPtrAllocator& allocator, const ClassObject* componentType, llvm::StringSaver& stringSaver)
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

    auto* result = create(allocator, 0, arrayFieldAreaSize, {}, {}, {},
                          stringSaver.save("[L" + componentType->getClassName() + ";"), nullptr);
    result->m_componentTypeAndIsPrimitive.setPointer(componentType);
    return result;
}

jllvm::ClassObject::ClassObject(std::uint32_t instanceSize, llvm::StringRef name)
    : ClassObject(instanceSize - sizeof(void*), {}, {}, {}, {}, name, nullptr)
{
    m_componentTypeAndIsPrimitive.setInt(true);
}

const jllvm::Field* jllvm::ClassObject::getField(llvm::StringRef fieldName, llvm::StringRef fieldType,
                                                 bool isStatic) const
{
    for (const ClassObject* curr = this; curr; curr = curr->getSuperClass())
    {
        const Field* iter = llvm::find_if(curr->getFields(),
                                          [&](const Field& field) {
                                              return field.isStatic() == isStatic && field.getName() == fieldName
                                                     && field.getType() == fieldType;
                                          });
        if (iter != curr->getFields().end())
        {
            return iter;
        }
    }
    return nullptr;
}

jllvm::ClassObject::ClassObject(std::size_t interfaceId, llvm::ArrayRef<Method> methods, llvm::ArrayRef<Field> fields,
                                llvm::ArrayRef<const ClassObject*> interfaces, llvm::StringRef className)
    : m_fieldAreaSize(0),
      m_methods(methods),
      m_fields(fields),
      m_interfaces(interfaces),
      m_className(className),
      m_superClassOrInterfaceId(interfaceId)
{
}

jllvm::ClassObject* jllvm::ClassObject::createInterface(llvm::BumpPtrAllocator& allocator, std::size_t interfaceId,
                                                        llvm::ArrayRef<Method> methods, llvm::ArrayRef<Field> fields,
                                                        llvm::ArrayRef<const ClassObject*> interfaces,
                                                        llvm::StringRef className)
{
    return new (allocator.Allocate<ClassObject>())
        ClassObject(interfaceId, arrayRefAlloc(allocator, methods), arrayRefAlloc(allocator, fields),
                    arrayRefAlloc(allocator, interfaces), className);
}

jllvm::ITable* jllvm::ITable::create(llvm::BumpPtrAllocator& allocator, std::size_t id, std::size_t iTableSlots)
{
    auto* result =
        new (allocator.Allocate(ITable::totalSizeToAlloc<VTableSlot>(iTableSlots), alignof(ITable))) ITable(id);
    std::memset(result->getTrailingObjects<VTableSlot>(), 0, sizeof(VTableSlot) * iTableSlots);
    return result;
}
