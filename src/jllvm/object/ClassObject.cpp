#include "ClassObject.hpp"

#include <llvm/ADT/Twine.h>

jllvm::ClassObject* jllvm::ClassObject::create(llvm::BumpPtrAllocator& allocator, std::size_t vTableSlots,
                                               std::uint32_t fieldAreaSize, llvm::ArrayRef<Method> methods,
                                               llvm::ArrayRef<Field> fields,
                                               llvm::ArrayRef<const ClassObject*> interfaces, llvm::StringRef className,
                                               const jllvm::ClassObject* superClass)
{
    auto arrayRefAlloc = [&](auto ref)
    {
        auto* storage = allocator.Allocate<typename decltype(ref)::value_type>(ref.size());
        llvm::copy(ref, storage);
        return llvm::ArrayRef{storage, ref.size()};
    };

    auto* result =
        new (allocator.Allocate(ClassObject::totalSizeToAlloc<VTableSlot>(vTableSlots), alignof(ClassObject)))
            ClassObject(fieldAreaSize, arrayRefAlloc(methods), arrayRefAlloc(fields), arrayRefAlloc(interfaces),
                        className, superClass);
    // Zero out vTable.
    std::memset(result->getVTable(), 0, vTableSlots * sizeof(VTableSlot));
    return result;
}

jllvm::ClassObject::ClassObject(std::int32_t fieldAreaSize, llvm::ArrayRef<Method> methods, llvm::ArrayRef<Field> fields,
                                llvm::ArrayRef<const ClassObject*> interfaces, llvm::StringRef className,
                                const jllvm::ClassObject* superClass)
    : m_fieldAreaSize(fieldAreaSize),
      m_methods(methods),
      m_fields(fields),
      m_interfaces(interfaces),
      m_className(className),
      m_superClass(superClass)
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
    : ClassObject(instanceSize - sizeof(void*), {}, {}, {}, name, nullptr)
{
    m_componentTypeAndIsPrimitive.setInt(true);
}
