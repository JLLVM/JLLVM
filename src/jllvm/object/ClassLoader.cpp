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
    if (ClassObject* result = forNameLoaded("L" + className + ";"))
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
        superClass = &forName("L" + *superClassName + ";");
    }

    llvm::SmallVector<ClassObject*> interfaces;
    for (llvm::StringRef iter : classFile.getInterfaces())
    {
        interfaces.push_back(&forName("L" + iter + ";"));
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
            llvm::StringRef descriptor = fieldInfo.getDescriptor(classFile);
            if (isReferenceDescriptor(descriptor))
            {
                fields.emplace_back(fieldInfo.getName(classFile), descriptor, m_allocateStatic());
                continue;
            }

            fields.emplace_back(fieldInfo.getName(classFile), descriptor);
            continue;
        }

        auto desc = parseFieldType(fieldInfo.getDescriptor(classFile));
        auto fieldSizeAndAlignment = match(
            desc,
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
                            instanceSize + sizeof(ObjectHeader));
        instanceSize += fieldSizeAndAlignment;
    }
    instanceSize = llvm::alignTo(instanceSize, alignof(ObjectHeader));

    ClassObject* result;

    if (classFile.isInterface())
    {
        result = ClassObject::createInterface(m_classAllocator, m_metaClassObject, m_interfaceIdCounter++, methods,
                                              fields, interfaces, className);
    }
    else
    {
        if (superClass)
        {
            interfaces.insert(interfaces.begin(), superClass);
        }
        result = ClassObject::create(m_classAllocator, m_metaClassObject, vTableAssignment.tableSize, instanceSize,
                                     methods, fields, interfaces, className, classFile.isAbstract());
    }
    m_mapping.insert({("L" + className + ";").str(), result});
    m_prepareClassObject(&classFile, *result);

    return *result;
}

jllvm::ClassObject* jllvm::ClassLoader::forNameLoaded(llvm::Twine fieldDescriptor)
{
    llvm::SmallString<32> str;
    llvm::StringRef classNameRef = fieldDescriptor.toStringRef(str);
    auto result = m_mapping.find(classNameRef);
    if (result != m_mapping.end())
    {
        return result->second;
    }

    if (classNameRef.front() != '[')
    {
        return nullptr;
    }

    // Extra optimization for loading array types. Since creating the class object for an array type has essentially
    // no side effects on the execution of JVM bytecode we can always create the array object eagerly as long as its
    // component type has been loaded. This leads to better code generation as no stubs or other similar code has to be
    // generated to load array class objects.
    // Get the first component type that is not an array.
    llvm::StringRef arrayTypesRemoved = classNameRef.drop_while([](char c) { return c == '['; });
    result = m_mapping.find(arrayTypesRemoved);
    if (result == m_mapping.end())
    {
        // If the component type is not loaded we have to lazy load the array object anyway.
        return nullptr;
    }

    // Otherwise we now just need to create all array objects for all dimensions that we stripped above.
    ClassObject* curr = result->second;
    std::size_t arrayTypesCount = classNameRef.size() - arrayTypesRemoved.size();
    for (std::size_t i = 1; i <= arrayTypesCount; i++)
    {
        curr = ClassObject::createArray(m_classAllocator, m_metaClassObject, curr, m_stringSaver);
        // We are moving from right to left in the array type name, therefore always stripping one less array type
        // descriptor ('['), from the class name.
        m_mapping.insert({classNameRef.drop_front(arrayTypesCount - i), curr});
    }
    return curr;
}

jllvm::ClassObject& jllvm::ClassLoader::forName(llvm::Twine fieldDescriptor)
{
    if (ClassObject* result = forNameLoaded(fieldDescriptor))
    {
        return *result;
    }

    llvm::StringRef className;
    llvm::SmallString<32> twineStorage;
    {
        fieldDescriptor.toVector(twineStorage);
        // Array type case.
        if (twineStorage.front() == '[')
        {
            const ClassObject& componentType = forName(llvm::StringRef(twineStorage).drop_front());
            ClassObject* arrayType =
                ClassObject::createArray(m_classAllocator, m_metaClassObject, &componentType, m_stringSaver);
            m_mapping.insert({twineStorage, arrayType});
            return *arrayType;
        }
        className = twineStorage;
        // Drop both the leading 'L' and trailing ';'. Primitive types are preregistered so always succeed in
        // 'forNameLoaded'. Array types are handled above, therefore only object types have to handled here.
        className = className.drop_front().drop_back();
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

    llvm::StringRef raw = result->getBuffer();
    ClassFile classFile = ClassFile::parseFromFile({raw.begin(), raw.end()}, m_stringSaver);
    return add(std::move(result));
}

jllvm::ClassLoader::ClassLoader(std::vector<std::string>&& classPaths,
                                llvm::unique_function<void(const ClassFile*, ClassObject&)>&& prepareClassObject,
                                llvm::unique_function<void**()> allocateStatic)
    : m_classPaths(std::move(classPaths)),
      m_prepareClassObject(std::move(prepareClassObject)),
      m_allocateStatic(std::move(allocateStatic))
{
    m_mapping.insert({"B", &m_byte});
    m_mapping.insert({"C", &m_char});
    m_mapping.insert({"D", &m_double});
    m_mapping.insert({"F", &m_float});
    m_mapping.insert({"I", &m_int});
    m_mapping.insert({"J", &m_long});
    m_mapping.insert({"S", &m_short});
    m_mapping.insert({"Z", &m_boolean});
    m_mapping.insert({"V", &m_void});
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
