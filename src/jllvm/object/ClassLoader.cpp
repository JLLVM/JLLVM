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

#include "ClassLoader.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>

#include <jllvm/class/ClassFile.hpp>
#include <jllvm/class/Descriptors.hpp>

#define DEBUG_TYPE "jvm"

namespace
{
struct TableAssignment
{
    llvm::DenseMap<const jllvm::MethodInfo*, std::uint32_t> methodToSlot;
    std::uint32_t tableSize;
};

TableAssignment assignTableSlots(const jllvm::ClassFile& classFile, const jllvm::ClassObject* superClass)
{
    std::uint32_t nextVTableSlot = (superClass && !classFile.isInterface()) ? superClass->getTableSize() : 0;
    llvm::DenseMap<const jllvm::MethodInfo*, std::uint32_t> assignment;
    for (const jllvm::MethodInfo& iter : classFile.getMethods())
    {
        // If the method can't be overwritten we don't need to assign it a v-table slot.
        if (!iter.needsVTableSlot(classFile))
        {
            continue;
        }
        assignment.insert({&iter, nextVTableSlot++});
    }
    return {std::move(assignment), nextVTableSlot};
}

jllvm::Visibility visibility(const jllvm::MethodInfo& methodInfo)
{
    if (methodInfo.isPrivate())
    {
        return jllvm::Visibility::Private;
    }
    if (methodInfo.isProtected())
    {
        return jllvm::Visibility::Protected;
    }
    if (methodInfo.isPublic())
    {
        return jllvm::Visibility::Public;
    }
    return jllvm::Visibility::Package;
}

} // namespace

jllvm::ClassObject& jllvm::ClassLoader::add(std::unique_ptr<llvm::MemoryBuffer>&& memoryBuffer)
{
    llvm::StringRef raw = m_memoryBuffers.emplace_back(std::move(memoryBuffer))->getBuffer();
    ClassFile& classFile = m_classFiles.emplace_back(ClassFile::parseFromFile({raw.begin(), raw.end()}, m_stringSaver));

    llvm::StringRef className = classFile.getThisClass();
    if (ClassObject* result = forNameLoaded(ObjectType(className)))
    {
        return *result;
    }
    LLVM_DEBUG({ llvm::dbgs() << "Creating class object for " << className << '\n'; });

    // Get super classes and interfaces but only in prepared states!
    // We have a bit of a chicken-egg situation going on here. The JVM spec requires super class and interface
    // initialization to only happen after the class object has been created and been marked as
    // "currently initializing". We can't create the class object before knowing its v-table size though, which
    // requires knowing about the super classes. We therefore only load super classes and interfaces in "prepared"
    // state, initializing them later after the class object has been created.
    ClassObject* superClass = nullptr;
    if (std::optional<llvm::StringRef> superClassName = classFile.getSuperClass())
    {
        superClass = &forName(ObjectType(*superClassName));
    }

    llvm::SmallVector<ClassObject*> interfaces;
    for (llvm::StringRef iter : classFile.getInterfaces())
    {
        interfaces.push_back(&forName(ObjectType(iter)));
    }

    TableAssignment vTableAssignment = assignTableSlots(classFile, superClass);

    llvm::SmallVector<Method> methods;
    for (const MethodInfo& methodInfo : classFile.getMethods())
    {
        std::optional<std::uint32_t> vTableSlot;
        if (auto result = vTableAssignment.methodToSlot.find(&methodInfo);
            result != vTableAssignment.methodToSlot.end())
        {
            vTableSlot = result->second;
        }
        methods.emplace_back(methodInfo.getName(classFile), methodInfo.getDescriptor(classFile), vTableSlot,
                             methodInfo.isStatic(), methodInfo.isFinal(), methodInfo.isNative(), visibility(methodInfo),
                             methodInfo.isAbstract());
    }

    llvm::SmallVector<Field> fields;
    std::size_t instanceSize = superClass ? superClass->getFieldAreaSize() : 0;
    for (const FieldInfo& fieldInfo : classFile.getFields())
    {
        if (fieldInfo.isStatic())
        {
            FieldType descriptor = fieldInfo.getDescriptor(classFile);
            if (descriptor.isReference())
            {
                fields.emplace_back(fieldInfo.getName(classFile), descriptor, m_allocateStatic(),
                                    fieldInfo.getAccessFlags());
                continue;
            }

            fields.emplace_back(fieldInfo.getName(classFile), descriptor, fieldInfo.getAccessFlags());
            continue;
        }

        auto fieldSizeAndAlignment = match(
            fieldInfo.getDescriptor(classFile),
            [](BaseType baseType) -> std::size_t
            {
                switch (baseType.getValue())
                {
                    case BaseType::Byte: return 1;
                    case BaseType::Char: return 2;
                    case BaseType::Double: return sizeof(double);
                    case BaseType::Float: return sizeof(float);
                    case BaseType::Int: return 4;
                    case BaseType::Long: return 8;
                    case BaseType::Short: return 2;
                    case BaseType::Boolean: return 1;
                    case BaseType::Void: break;
                }
                llvm_unreachable("Field can't be void");
            },
            [](const ObjectType&) { return sizeof(void*); }, [](const ArrayType&) { return sizeof(void*); });
        instanceSize = llvm::alignTo(instanceSize, fieldSizeAndAlignment);
        fields.emplace_back(fieldInfo.getName(classFile), fieldInfo.getDescriptor(classFile),
                            instanceSize + sizeof(ObjectHeader), fieldInfo.getAccessFlags());
        instanceSize += fieldSizeAndAlignment;
    }
    instanceSize = llvm::alignTo(instanceSize, alignof(ObjectHeader));

    ClassObject* result;

    if (classFile.isInterface())
    {
        result = ClassObject::createInterface(m_classAllocator, m_metaClassObject, m_interfaceIdCounter++, methods,
                                              fields, interfaces, classFile);
    }
    else
    {
        if (superClass)
        {
            interfaces.insert(interfaces.begin(), superClass);
        }
        result = ClassObject::create(m_classAllocator, m_metaClassObject, vTableAssignment.tableSize, instanceSize,
                                     methods, fields, interfaces, classFile);
    }
    m_mapping.insert({ObjectType(className), result});
    m_prepareClassObject(&classFile, *result);

    return *result;
}

jllvm::ClassObject* jllvm::ClassLoader::forNameLoaded(FieldType fieldType)
{
    auto result = m_mapping.find(fieldType);
    if (result != m_mapping.end())
    {
        return result->second;
    }

    // Extra optimization for loading array types. Since creating the class object for an array type has essentially
    // no side effects on the execution of JVM bytecode we can always create the array object eagerly as long as its
    // component type has been loaded. This leads to better code generation as no stubs or other similar code has to be
    // generated to load array class objects.
    // Get the first component type that is not an array.

    std::size_t arrayTypesCount = 0;
    while (auto arrayType = get_if<ArrayType>(&fieldType))
    {
        arrayTypesCount++;
        fieldType = arrayType->getComponentType();
    }
    if (arrayTypesCount == 0)
    {
        return nullptr;
    }
    result = m_mapping.find(fieldType);
    if (result == m_mapping.end())
    {
        // If the component type is not loaded we have to lazy load the array object anyway.
        return nullptr;
    }

    // Otherwise we now just need to create all array objects for all dimensions that we stripped above.
    ClassObject* curr = result->second;
    for (std::size_t i = 1; i <= arrayTypesCount; i++)
    {
        curr = ClassObject::createArray(m_classAllocator, m_metaClassObject, curr, m_stringSaver);
        m_mapping.insert({curr->getDescriptor(), curr});
    }
    return curr;
}

jllvm::ClassObject& jllvm::ClassLoader::forName(FieldType fieldType)
{
    if (ClassObject* result = forNameLoaded(fieldType))
    {
        return *result;
    }

    llvm::StringRef className;
    {
        // Array type case.
        if (auto arrayTypeDesc = get_if<ArrayType>(&fieldType))
        {
            const ClassObject& componentType = forName(arrayTypeDesc->getComponentType());
            ClassObject* arrayType =
                ClassObject::createArray(m_classAllocator, m_metaClassObject, &componentType, m_stringSaver);
            m_mapping.insert({arrayType->getDescriptor(), arrayType});
            return *arrayType;
        }
        className = get<ObjectType>(fieldType).getClassName();
    }

    std::unique_ptr<llvm::MemoryBuffer> result;
    for (llvm::StringRef classPath : m_classPaths)
    {
        llvm::SmallString<64> temp = classPath;
        llvm::sys::path::append(temp, className);
        temp += ".class";
        if (auto buffer = llvm::MemoryBuffer::getFile(temp))
        {
            result = std::move(*buffer);
            break;
        }
    }

    if (!result)
    {
        llvm::report_fatal_error("No *.class file found for class " + className);
    }

    LLVM_DEBUG({ llvm::dbgs() << "Loaded " << result->getBufferIdentifier() << " from class path\n"; });
    return add(std::move(result));
}

jllvm::ClassLoader::ClassLoader(std::vector<std::string>&& classPaths,
                                llvm::unique_function<void(const ClassFile*, ClassObject&)>&& prepareClassObject,
                                llvm::unique_function<void**()> allocateStatic)
    : m_classPaths(std::move(classPaths)),
      m_prepareClassObject(std::move(prepareClassObject)),
      m_allocateStatic(std::move(allocateStatic))
{
    m_mapping.try_emplace("B", &m_byte);
    m_mapping.try_emplace("C", &m_char);
    m_mapping.try_emplace("D", &m_double);
    m_mapping.try_emplace("F", &m_float);
    m_mapping.try_emplace("I", &m_int);
    m_mapping.try_emplace("J", &m_long);
    m_mapping.try_emplace("S", &m_short);
    m_mapping.try_emplace("Z", &m_boolean);
    m_mapping.try_emplace("V", &m_void);
}

jllvm::ClassObject& jllvm::ClassLoader::loadBootstrapClasses()
{
    m_metaClassObject = &forName("Ljava/lang/Class;");

    // With the meta class object loaded we can update all so far loaded class objects to be of type 'Class'.
    // This includes 'Class' itself.
    for (ClassObject* classObject : llvm::make_second_range(m_mapping))
    {
        classObject->getObjectHeader().classObject = m_metaClassObject;
    }
    return *m_metaClassObject;
}

jllvm::ClassLoader::~ClassLoader()
{
    auto range = llvm::make_second_range(m_mapping);
    std::destroy(range.begin(), range.end());
}
