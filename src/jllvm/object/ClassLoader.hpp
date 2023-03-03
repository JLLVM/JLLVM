#pragma once

#include <llvm/ADT/FunctionExtras.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/StringSaver.h>

#include <list>

#include <jllvm/class/ClassFile.hpp>

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
    llvm::unique_function<void(const ClassFile*, ClassObject* classObject)> m_classObjectCreated;
    llvm::unique_function<void**()> m_allocateStatic;

    ClassObject m_byte{sizeof(std::uint8_t), "B"};
    ClassObject m_char{sizeof(std::int16_t), "C"};
    ClassObject m_double{sizeof(double), "D"};
    ClassObject m_float{sizeof(float), "F"};
    ClassObject m_int{sizeof(std::int32_t), "I"};
    ClassObject m_long{sizeof(std::int64_t), "J"};
    ClassObject m_short{sizeof(std::int16_t), "S"};
    ClassObject m_boolean{sizeof(bool), "Z"};
    ClassObject m_void{0, "V"};

public:
    /// Constructs a class loader with 'classPaths', which are all directories that class files will be searched for,
    /// 'classFileLoaded' which is a callback called when a class object has successfully been constructed from the
    /// given class file, and 'allocateStatic' which should allocate and return 'pointer sized' storage for any
    /// static variables of reference type.
    ClassLoader(std::vector<std::string>&& classPaths,
                llvm::unique_function<void(const ClassFile*, ClassObject* classObject)>&& classFileLoaded,
                llvm::unique_function<void**()> allocateStatic);

    /// Loads the class object for the given class file. This may also load transitive dependencies of the class file.#
    /// Currently aborts if a class file could not be loaded.
    const ClassObject& add(std::unique_ptr<llvm::MemoryBuffer>&& memoryBuffer);

    /// Returns the class object for 'fieldDescriptor', which must be a valid field descriptor, loading it and
    /// transitive dependencies if required. Currently aborts if a class file could not be loaded.
    const ClassObject& forName(llvm::Twine fieldDescriptor);

    /// Returns the class object for 'fieldDescriptor', which must be a valid field descriptor,
    /// if it has been loaded previously. Null otherwise.
    const ClassObject* forNameLoaded(llvm::Twine fieldDescriptor);
};

} // namespace jllvm
