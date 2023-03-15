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

const jllvm::ClassObject& jllvm::ClassLoader::add(std::unique_ptr<llvm::MemoryBuffer>&& memoryBuffer)
{
    llvm::StringRef raw = m_memoryBuffers.emplace_back(std::move(memoryBuffer))->getBuffer();
    ClassFile& classFile = m_classFiles.emplace_back(ClassFile::parseFromFile({raw.begin(), raw.end()}, m_stringSaver));

    llvm::StringRef className = classFile.getThisClass();
    if (const ClassObject* result = forNameLoaded("L" + className + ";"))
    {
        return *result;
    }
    LLVM_DEBUG({ llvm::dbgs() << "Creating class object for " << className << '\n'; });

    const ClassObject* superClass = nullptr;
    if (std::optional<llvm::StringRef> superClassName = classFile.getSuperClass())
    {
        superClass = &forName("L" + *superClassName + ";");
    }

    llvm::SmallVector<const ClassObject*> interfaces;
    for (llvm::StringRef iter : classFile.getInterfaces())
    {
        interfaces.push_back(&forName("L" + iter + ";"));
    }

    VTableAssignment vTableAssignment = assignVTableSlots(classFile, superClass);

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
                switch (baseType)
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

    auto* result = ClassObject::create(m_classAllocator, vTableAssignment.vTableCount, instanceSize, methods, fields,
                                       interfaces, className, superClass);

    m_mapping.insert({("L" + className + ";").str(), result});

    m_classObjectCreated(&classFile, result);

    return *result;
}

const jllvm::ClassObject* jllvm::ClassLoader::forNameLoaded(llvm::Twine className)
{
    llvm::SmallString<32> str;
    auto result = m_mapping.find(className.toStringRef(str));
    if (result != m_mapping.end())
    {
        return result->second;
    }
    return nullptr;
}

const jllvm::ClassObject& jllvm::ClassLoader::forName(llvm::Twine fieldDescriptor)
{
    if (const ClassObject* result = forNameLoaded(fieldDescriptor))
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
    return add(std::move(result));
}

jllvm::ClassLoader::ClassLoader(std::vector<std::string>&& classPaths,
                                llvm::unique_function<void(const ClassFile*, ClassObject*)>&& classFileLoaded,
                                llvm::unique_function<void**()> allocateStatic)
    : m_classPaths(std::move(classPaths)),
      m_classObjectCreated(std::move(classFileLoaded)),
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
