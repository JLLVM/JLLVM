#pragma once

#include <llvm/ADT/FunctionExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/StringSaver.h>

#include <jllvm/class/ClassFile.hpp>

#include <list>

#include "ClassObject.hpp"

namespace jllvm
{
/// The default bootstrap class loader capable of creating class objects from class files. It also contains
/// the builtin class objects.
class ClassLoader
{
    llvm::BumpPtrAllocator m_classAllocator;
    llvm::StringMap<ClassObject*> m_mapping;

    llvm::BumpPtrAllocator m_stringAllocator;
    llvm::StringSaver m_stringSaver{m_stringAllocator};
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> m_memoryBuffers;
    std::list<ClassFile> m_classFiles;

    std::vector<std::string> m_classPaths;
    llvm::unique_function<void(const ClassFile*, ClassObject&)> m_prepareClassObject;
    llvm::unique_function<void**()> m_allocateStatic;
    std::size_t m_interfaceIdCounter = 0;

    ClassObject m_byte{sizeof(std::uint8_t), "B"};
    ClassObject m_char{sizeof(std::int16_t), "C"};
    ClassObject m_double{sizeof(double), "D"};
    ClassObject m_float{sizeof(float), "F"};
    ClassObject m_int{sizeof(std::int32_t), "I"};
    ClassObject m_long{sizeof(std::int64_t), "J"};
    ClassObject m_short{sizeof(std::int16_t), "S"};
    ClassObject m_boolean{sizeof(bool), "Z"};
    ClassObject m_void{0, "V"};

    ClassObject* m_metaClassObject = nullptr;


public:
    /// Constructs a class loader with 'classPaths', which are all directories that class files will be searched for.
    /// 'initializeClassObject' is called for the initialization step of a class object and should at the very least
    /// call the class initialization function of the class object.
    /// 'classFileLoaded' is called when a class file has been loaded and is being used by the class loader.
    /// 'allocateStatic' should allocate and return 'pointer sized' storage for any static variables of reference type.
    ClassLoader(std::vector<std::string>&& classPaths,
                llvm::unique_function<void(const ClassFile*, ClassObject&)>&& prepareClassObject,
                llvm::unique_function<void**()> allocateStatic);

    /// Loads the class object for the given class file. This may also load transitive dependencies
    /// of the class file. Currently aborts if a class file could not be loaded.
    ClassObject& add(std::unique_ptr<llvm::MemoryBuffer>&& memoryBuffer);

    /// Returns the class object for 'fieldDescriptor', which must be a valid field descriptor, loading it and
    /// transitive dependencies if required. Currently aborts if a class file could not be loaded.
    ClassObject& forName(llvm::Twine fieldDescriptor);

    /// Returns the class object for 'fieldDescriptor', which must be a valid field descriptor,
    /// if it has been loaded previously. Null otherwise.
    ClassObject* forNameLoaded(llvm::Twine fieldDescriptor);

    /// Loads java classes required to boot up the VM. This a separate method and not executed as part of the
    /// constructor as it requires the VM to already be ready to execute JVM Bytecode (one that does not depend on the
    /// bootstrap classes) and to have at the very least initialized the builtin native methods of the bootstrap
    /// classes.
    /// Returns the meta class object.
    ClassObject& loadBootstrapClasses();
};

} // namespace jllvm
