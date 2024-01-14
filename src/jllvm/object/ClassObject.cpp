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

#include "ClassObject.hpp"

#include <llvm/ADT/Twine.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

namespace
{
template <class Range>
auto arrayRefAlloc(llvm::BumpPtrAllocator& allocator, const Range& ref)
{
    auto* storage = allocator.Allocate<typename Range::value_type>(ref.size());
    llvm::copy(ref, storage);
    return llvm::MutableArrayRef{storage, ref.size()};
}
} // namespace

std::string jllvm::Method::prettySignature() const
{
    std::string className = m_classObject->getDescriptor().pretty();
    std::string returnType = m_type.returnType().pretty();
    std::string paramTypes;
    llvm::raw_string_ostream paramStream{paramTypes};

    llvm::interleaveComma(llvm::map_range(m_type.parameters(), std::mem_fn(&FieldType::pretty)), paramStream);

    return llvm::formatv("{0} {1}.{2}({3})", returnType, className, m_name, paramTypes).str();
}

jllvm::ClassObject* jllvm::ClassObject::create(llvm::BumpPtrAllocator& allocator, const ClassObject* metaClass,
                                               std::uint32_t vTableSlots, std::uint32_t fieldAreaSize,
                                               llvm::ArrayRef<Method> methods, llvm::ArrayRef<Field> fields,
                                               llvm::ArrayRef<ClassObject*> bases, const ClassFile& classFile)
{
    bool isAbstract = classFile.isAbstract();
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
        if (iter.isStatic() || !iter.getType().isReference())
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
    llvm::MutableArrayRef<Method> methodsAlloc = arrayRefAlloc(allocator, methods);
    auto* result = new (storage) ClassObject(
        metaClass, vTableSlots, fieldAreaSize, NonOwningFrozenSet(methodsAlloc, allocator),
        NonOwningFrozenSet(arrayRefAlloc(allocator, fields), allocator), arrayRefAlloc(allocator, bases),
        arrayRefAlloc(allocator, iTables), classFile.getThisClass(), classFile, arrayRefAlloc(allocator, gcMask));
    for (Method& method : methodsAlloc)
    {
        method.setClassObject(result);
    }
    // Zero out vTable.
    std::fill(result->getVTable().begin(), result->getVTable().end(), nullptr);
    return result;
}

jllvm::ClassObject::ClassObject(const ClassObject* metaClass, std::uint32_t vTableSlots, std::int32_t fieldAreaSize,
                                const NonOwningFrozenSet<Method>& methods, const NonOwningFrozenSet<Field>& fields,
                                llvm::ArrayRef<ClassObject*> bases, llvm::ArrayRef<ITable*> iTables,
                                llvm::StringRef className, const ClassFile& classFile,
                                llvm::ArrayRef<std::uint32_t> gcMask)
    : m_objectHeader(metaClass),
      m_fieldAreaSize(fieldAreaSize),
      m_tableSize(vTableSlots),
      m_methods(methods),
      m_fields(fields),
      m_bases(bases),
      m_iTables(iTables),
      m_className(className),
      m_gcMask(gcMask),
      m_classFile(&classFile)
{
}

jllvm::ClassObject::ClassObject(const jllvm::ClassObject* metaClass, std::int32_t fieldAreaSize,
                                llvm::StringRef className)
    : m_objectHeader(metaClass), m_fieldAreaSize(fieldAreaSize), m_className(className)
{
}

jllvm::ClassObject* jllvm::ClassObject::createArray(llvm::BumpPtrAllocator& allocator, ClassObject* objectClass,
                                                    const ClassObject* componentType, llvm::StringSaver& stringSaver,
                                                    llvm::ArrayRef<ClassObject*> arrayBases)
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

    std::uint32_t vTableSlots = objectClass->getTableSize();
    auto* result =
        new (allocator.Allocate(ClassObject::totalSizeToAlloc<VTableSlot>(vTableSlots), alignof(ClassObject)))
            ClassObject(objectClass->getClass(), arrayFieldAreaSize,
                        stringSaver.save(FieldType(ArrayType(componentType->getDescriptor())).textual()));
    result->m_componentTypeOrInterfaceId = componentType;
    result->m_initialized = InitializationStatus::Initialized;
    result->m_tableSize = vTableSlots;
    result->m_bases = arrayBases;
    std::fill(result->getVTable().begin(), result->getVTable().end(), nullptr);
    return result;
}

jllvm::ClassObject::ClassObject(std::uint32_t instanceSize, llvm::StringRef name)
    : ClassObject(nullptr, instanceSize - sizeof(ObjectHeader), name)
{
    // NOLINTBEGIN(*-prefer-member-initializer): https://github.com/llvm/llvm-project/issues/52818
    m_isPrimitive = true;
    m_initialized = InitializationStatus::Initialized;
    // NOLINTEND(*-prefer-member-initializer)
}

jllvm::ClassObject::ClassObject(const ClassObject* metaClass, std::size_t interfaceId,
                                const NonOwningFrozenSet<Method>& methods, const NonOwningFrozenSet<Field>& fields,
                                llvm::ArrayRef<ClassObject*> interfaces, llvm::StringRef className,
                                const ClassFile& classFile)
    : m_objectHeader(metaClass),
      m_fieldAreaSize(0),
      m_tableSize(llvm::count_if(methods, [](const Method& method) { return method.getTableSlot(); })),
      m_methods(methods),
      m_fields(fields),
      m_bases(interfaces),
      m_className(className),
      m_componentTypeOrInterfaceId(interfaceId),
      m_classFile(&classFile)
{
}

jllvm::ClassObject* jllvm::ClassObject::createInterface(llvm::BumpPtrAllocator& allocator, const ClassObject* metaClass,
                                                        std::size_t interfaceId, llvm::ArrayRef<Method> methods,
                                                        llvm::ArrayRef<Field> fields,
                                                        llvm::ArrayRef<ClassObject*> interfaces,
                                                        const ClassFile& classFile)
{
    llvm::MutableArrayRef<jllvm::Method> methodsAllocated = arrayRefAlloc(allocator, methods);
    auto* result = new (allocator.Allocate<ClassObject>())
        ClassObject(metaClass, interfaceId, NonOwningFrozenSet(methodsAllocated, allocator),
                    NonOwningFrozenSet(arrayRefAlloc(allocator, fields), allocator),
                    arrayRefAlloc(allocator, interfaces), classFile.getThisClass(), classFile);
    for (Method& method : methodsAllocated)
    {
        method.setClassObject(result);
    }
    return result;
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
        if (other->isInterface())
        {
            // If T is an interface type, then T must be one of the interfaces implemented by arrays.
            return llvm::is_contained(getAllInterfaces(), other);
        }

        // Strip array types and check that the component types are compatible.
        const ClassObject* curr = this;
        while (curr->isArray() && other->isArray())
        {
            curr = curr->getComponentType();
            other = other->getComponentType();
        }
        if (curr->isArray())
        {
            if (other->isClass())
            {
                // If T is a class type, then T must be Object.
                // Object is easy to identify as it is a normal class with no super class.
                return !other->getSuperClass();
            }

            // Not the same depth of array types.
            return false;
        }
        return curr->wouldBeInstanceOf(other);
    }

    if (other->isInterface())
    {
        // If T is an interface type, then S must implement interface T.
        return llvm::is_contained(getAllInterfaces(), other);
    }
    // If T is a class type, then S must be a subclass of T.
    return llvm::is_contained(getSuperClasses(), other);
}

namespace
{
bool canOverride(const jllvm::ClassObject* derivedClass, const jllvm::Method& base)
{
    // https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-5.html#jvms-5.4.5

    switch (base.getVisibility())
    {
        case jllvm::Visibility::Private: return false;
        case jllvm::Visibility::Public:
        case jllvm::Visibility::Protected: return true;
        case jllvm::Visibility::Package:
            // 5.4.5 mA is marked neither ACC_PUBLIC nor ACC_PROTECTED nor ACC_PRIVATE, and either (a) the declaration
            // of mA appears in the same run-time package as the declaration of mC.
            // TODO: I am pretty sure this is not how the spec defines packages, but it'll do for now.
            if (derivedClass->getPackageName() == base.getClassObject()->getPackageName())
            {
                return true;
            }

            // TODO: 5.4.5 b)
            llvm_unreachable("NOT YET IMPLEMENTED");
    }
    llvm_unreachable("All visibilities handled");
}
} // namespace

const jllvm::Method& jllvm::ClassObject::methodSelection(const Method& resolvedMethod) const
{
    // https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-5.html#jvms-5.4.6 Step 1
    if (resolvedMethod.getVisibility() == jllvm::Visibility::Private)
    {
        return resolvedMethod;
    }

    // Step 2

    // If C contains a declaration of an instance method m that can override mR (§5.4.5), then m is the selected method.

    // Otherwise, if C has a superclass, a search for a declaration of an instance method that can override mR is
    // performed, starting with the direct superclass of C and continuing with the direct superclass of that class, and
    // so forth, until a method is found or no further superclasses exist. If a method is found, it is the selected
    // method.
    for (const ClassObject* curr : getSuperClasses())
    {
        const Method* result = curr->getMethod(resolvedMethod.getName(), resolvedMethod.getType(),
                                               [&](const Method& method)
                                               { return !method.isStatic() && canOverride(curr, resolvedMethod); });
        if (result)
        {
            return *result;
        }
    }

    // Otherwise, the maximally-specific superinterface methods of C are determined (§5.4.3.3). If exactly one matches
    // mR's name and descriptor and is not abstract, then it is the selected method.

    // A maximally-specific superinterface method of a class or interface C for a particular method name and descriptor
    // is any method for which all of the following are true:
    //
    // The method is declared in a superinterface (direct or indirect) of C.
    //
    // The method is declared with the specified name and descriptor.
    //
    // The method has neither its ACC_PRIVATE flag nor its ACC_STATIC flag set.
    //
    // Where the method is declared in interface I, there exists no other maximally-specific superinterface method of C
    // with the specified name and descriptor that is declared in a subinterface of I.

    for (const ClassObject* interface : maximallySpecificInterfaces())
    {
        const Method* result =
            interface->getMethod(resolvedMethod.getName(), resolvedMethod.getType(),
                                 [&](const jllvm::Method& method)
                                 {
                                     return !method.isStatic() && method.getVisibility() != jllvm::Visibility::Private
                                            && !method.isAbstract() && canOverride(interface, resolvedMethod);
                                 });
        if (result)
        {
            return *result;
        }
    }

    llvm_unreachable("should not be possible");
}

const jllvm::Method* jllvm::ClassObject::methodResolution(llvm::StringRef methodName, MethodType methodType) const
{
    // https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-5.html#jvms-5.4.3.3

    // Otherwise, method resolution attempts to locate the referenced method
    // in C and its superclasses:

    // Otherwise, if C declares a method with the name and descriptor
    // specified by the method reference, method lookup succeeds.

    // Otherwise, if C has a superclass, step 2 of method resolution is
    // recursively invoked on the direct superclass of C.
    if (const Method* iter = getMethodSuper(methodName, methodType))
    {
        return iter;
    }

    // Otherwise, method resolution attempts to locate the referenced method
    // in the superinterfaces of the specified class C:

    // If the maximally-specific superinterface methods of C for the name
    // and descriptor specified by the method reference include exactly one
    // method that does not have its ACC_ABSTRACT flag set, then this method
    // is chosen and method lookup succeeds.
    for (const ClassObject* interface : maximallySpecificInterfaces())
    {
        if (const Method* method =
                interface->getMethod(methodName, methodType, std::not_fn(std::mem_fn(&Method::isAbstract))))
        {
            return method;
        }
    }

    // Otherwise, if any superinterface of C declares a method with the name and descriptor specified by the method
    // reference that has neither its ACC_PRIVATE flag nor its ACC_STATIC flag set, one of these is arbitrarily
    // chosen and method lookup succeeds.
    for (const ClassObject* interface : getAllInterfaces())
    {
        if (const Method* method =
                interface->getMethod(methodName, methodType,
                                     [](const Method& method)
                                     { return !method.isStatic() && method.getVisibility() != Visibility::Private; }))
        {
            return method;
        }
    }

    return nullptr;
}

const jllvm::Method* jllvm::ClassObject::interfaceMethodResolution(llvm::StringRef methodName, MethodType methodType,
                                                                   const ClassObject* objectClass) const
{
    // https://docs.oracle.com/javase/specs/jvms/se17/html/jvms-5.html#jvms-5.4.3.4

    // Otherwise, if C declares a method with the name and descriptor specified by the interface method
    // reference, method lookup succeeds.
    if (const Method* method = getMethod(methodName, methodType))
    {
        return method;
    }

    // Otherwise, if the class Object declares a method with the name and descriptor specified by the
    // interface method reference, which has its ACC_PUBLIC flag set and does not have its ACC_STATIC flag
    // set, method lookup succeeds.
    if (const Method* method = objectClass->getMethod(
            methodName, methodType,
            [](const Method& method) { return !method.isStatic() && method.getVisibility() == Visibility::Public; }))
    {
        return method;
    }

    // Otherwise, if the maximally-specific superinterface methods (§5.4.3.3) of C for the name and
    // descriptor specified by the method reference include exactly one method that does not have its
    // ACC_ABSTRACT flag set, then this method is chosen and method lookup succeeds.
    for (const ClassObject* interface : maximallySpecificInterfaces())
    {
        if (const Method* method =
                interface->getMethod(methodName, methodType, std::not_fn(std::mem_fn(&Method::isAbstract))))
        {
            return method;
        }
    }

    // Otherwise, if any superinterface of C declares a method with the name and descriptor specified by the method
    // reference that has neither its ACC_PRIVATE flag nor its ACC_STATIC flag set, one of these is arbitrarily chosen
    // and method lookup succeeds.
    for (const ClassObject* interface : getAllInterfaces())
    {
        const Method* method = interface->getMethod(
            methodName, methodType,
            [](const Method& method) { return !method.isStatic() && method.getVisibility() != Visibility::Private; });
        if (method)
        {
            return method;
        }
    }

    return nullptr;
}

const jllvm::Method* jllvm::ClassObject::specialMethodResolution(llvm::StringRef methodName, MethodType methodType,
                                                                 const ClassObject* objectClass,
                                                                 const ClassObject* callContext) const
{
    // The named method is resolved (§5.4.3.3, §5.4.3.4).
    const Method* resolvedMethod = isInterface() ? interfaceMethodResolution(methodName, methodType, objectClass) :
                                                   methodResolution(methodName, methodType);
    const ClassObject* resolvedClass = resolvedMethod->getClassObject();

    // If all of the following are true, let C be the direct superclass of the current class:
    //
    // The resolved method is not an instance initialization method (§2.9.1).
    //
    // The symbolic reference names a class (not an interface), and that class is a superclass of the current class.
    //
    // The ACC_SUPER flag is set for the class file (§4.1).
    if (!callContext || resolvedMethod->isObjectConstructor() || !resolvedClass->isClass()
        || !llvm::is_contained(callContext->getSuperClasses(/*includeThis=*/false), resolvedClass))
    {
        return resolvedMethod;
    }

    // What follows in the spec is essentially an interface or method resolution but with 'resolvedClass' as the new
    // class.
    resolvedClass = callContext->getSuperClass();
    return resolvedClass->isInterface() ?
               resolvedClass->interfaceMethodResolution(methodName, methodType, objectClass) :
               resolvedClass->methodResolution(methodName, methodType);
}
