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

struct VTableAssignment
{
    llvm::DenseMap<const jllvm::MethodInfo*, std::uint16_t> methodToSlot;
    std::uint16_t vTableCount;
};

VTableAssignment assignVTableSlots(const jllvm::ClassFile& classFile, const jllvm::ClassObject* superClass)
{
    using namespace jllvm;

    llvm::DenseMap<std::pair<llvm::StringRef, llvm::StringRef>, std::pair<std::uint16_t, const Method*>> map;
    while (superClass)
    {
        for (const Method& iter : superClass->getMethods())
        {
            if (auto slot = iter.getVTableSlot())
            {
                map.insert({{iter.getName(), iter.getType()}, {*slot, &iter}});
            }
        }
        superClass = superClass->getSuperClass();
    }
    std::uint16_t vTableCount = 0;
    if (!map.empty())
    {
        auto range = llvm::make_first_range(llvm::make_second_range(map));
        vTableCount = *std::max_element(range.begin(), range.end()) + 1;
    }

    llvm::DenseMap<const jllvm::MethodInfo*, std::uint16_t> assignment;
    for (const MethodInfo& iter : classFile.getMethods())
    {
        // If the method can never overwrite we don't need to assign it a v-table slot.
        if (!iter.canOverwrite(classFile))
        {
            continue;
        }

        auto result = map.find({iter.getName(classFile), iter.getDescriptor(classFile)});
        if (result == map.end())
        {
            // The method does not overwrite any subclass methods. We need to create a v-table slot if it may
            // potentially be overwritten however.
            if (iter.canBeOverwritten(classFile))
            {
                assignment.insert({&iter, vTableCount++});
            }
            continue;
        }

        auto [superClassSlot, superClassMethod] = result->second;
        if (superClassMethod->getVisibility() != jllvm::Visibility::Package)
        {
            assignment.insert({&iter, superClassSlot});
            continue;
        }

        // TODO: We need two v-table slots in the case of overwriting package default v-table slots.
        //       See 5.4.5 in the JVM spec. This is more complicated.
        llvm::report_fatal_error("Not yet implemented");
    }
    return {std::move(assignment), vTableCount};
}

VTableAssignment assignITableSlots(const jllvm::ClassFile& classFile)
{
    llvm::DenseMap<const jllvm::MethodInfo*, std::uint16_t> methodToSlot;

    for (const jllvm::MethodInfo& iter : classFile.getMethods())
    {
        if (iter.isStatic())
        {
            continue;
        }
        methodToSlot.insert({&iter, methodToSlot.size()});
    }

    return {std::move(methodToSlot), 0};
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
    m_classFileLoaded(&classFile);

    // Get super classes and interfaces but only in prepared states!
    // We have a bit of a chicken-egg situation going on here. The JVM spec requires super class and interface
    // initialization to only happen after the class object has been created and been marked as
    // "currently initializing". We can't create the class object before knowing its v-table size though, which
    // requires knowing about the super classes. We therefore only load super classes and interfaces in "prepared"
    // state, initializing them later after the class object has been created.
    ClassObject* superClass = nullptr;
    if (std::optional<llvm::StringRef> superClassName = classFile.getSuperClass())
    {
        superClass = &forName("L" + *superClassName + ";", State::Prepared);
    }

    llvm::SmallVector<ClassObject*> interfaces;
    for (llvm::StringRef iter : classFile.getInterfaces())
    {
        interfaces.push_back(&forName("L" + iter + ";", State::Prepared));
    }

    VTableAssignment vTableAssignment =
        classFile.isInterface() ? assignITableSlots(classFile) : assignVTableSlots(classFile, superClass);

    llvm::SmallVector<Method> methods;
    for (const MethodInfo& methodInfo : classFile.getMethods())
    {
        std::optional<std::uint16_t> vTableSlot;
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
                    case BaseType::Void: llvm_unreachable("Field can't be void");
                }
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
        result = ClassObject::createInterface(m_classAllocator, m_interfaceIdCounter++, methods, fields, interfaces,
                                              className);
    }
    else
    {
        result = ClassObject::create(m_classAllocator, vTableAssignment.vTableCount, instanceSize, methods, fields,
                                     interfaces, className, superClass);
    }
    m_mapping.insert({("L" + className + ";").str(), result});

    // 5.5 Initialization, step 7: Initialize super class and interfaces recursively.
    if (superClass)
    {
        initialize(*superClass);
    }
    llvm::for_each(interfaces, [this](ClassObject* interface) { initialize(*interface); });

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
        curr = ClassObject::createArray(m_classAllocator, curr, m_stringSaver);
        // We are moving from right to left in the array type name, therefore always stripping one less array type
        // descriptor ('['), from the class name.
        m_mapping.insert({classNameRef.drop_front(arrayTypesCount - i), curr});
    }
    return curr;
}

jllvm::ClassObject& jllvm::ClassLoader::forName(llvm::Twine fieldDescriptor, State state)
{
    if (ClassObject* result = forNameLoaded(fieldDescriptor))
    {
        if (state == State::Initialized)
        {
            // If the state of the class object has to be initialized, initialize it.
            initialize(*result);
        }
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
            ClassObject* arrayType = ClassObject::createArray(m_classAllocator, &componentType, m_stringSaver);
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
        llvm::report_fatal_error("No *.class file found for clss " + className);
    }

    LLVM_DEBUG({ llvm::dbgs() << "Loaded " << result->getBufferIdentifier() << " from class path\n"; });

    llvm::StringRef raw = result->getBuffer();
    ClassFile classFile = ClassFile::parseFromFile({raw.begin(), raw.end()}, m_stringSaver);
    ClassObject& classObject = add(std::move(result));
    switch (state)
    {
        case State::Prepared: m_uninitialized.insert(&classObject); break;
        case State::Initialized: m_initializeClassObject(&classObject); break;
    }
    return classObject;
}

jllvm::ClassLoader::ClassLoader(std::vector<std::string>&& classPaths,
                                llvm::unique_function<void(ClassObject*)>&& initializeClassObject,
                                llvm::unique_function<void(const ClassFile*)>&& classFileLoaded,
                                llvm::unique_function<void**()> allocateStatic)
    : m_classPaths(std::move(classPaths)),
      m_initializeClassObject(std::move(initializeClassObject)),
      m_classFileLoaded(std::move(classFileLoaded)),
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

const jllvm::ClassObject& jllvm::ClassLoader::addAndInitialize(std::unique_ptr<llvm::MemoryBuffer>&& memoryBuffer)
{
    ClassObject& classObject = add(std::move(memoryBuffer));
    m_initializeClassObject(&classObject);
    return classObject;
}
