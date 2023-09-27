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
    llvm::DenseMap<FieldType, ClassObject*> m_mapping;

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
    /// 'prepareClassObject' is called when a class file has been loaded and a class object derived from it. This can
    /// be used to register the class object or prepare it in an additional action outside of the class loader.
    /// 'allocateStatic' should allocate and return 'pointer sized' storage for any static variables of reference type.
    ClassLoader(std::vector<std::string>&& classPaths,
                llvm::unique_function<void(const ClassFile*, ClassObject&)>&& prepareClassObject,
                llvm::unique_function<void**()> allocateStatic);

    ~ClassLoader();

    ClassLoader(const ClassLoader&) = delete;
    ClassLoader& operator=(const ClassLoader&) = delete;
    ClassLoader(ClassLoader&&) noexcept = delete;
    ClassLoader& operator=(ClassLoader&&) noexcept = delete;

    /// Loads the class object for the given class file. This may also load transitive dependencies
    /// of the class file. Currently aborts if a class file could not be loaded.
    ClassObject& add(std::unique_ptr<llvm::MemoryBuffer>&& memoryBuffer);

    /// Returns the class object for 'fieldDescriptor', which must be a valid field descriptor, loading it and
    /// transitive dependencies if required. Currently aborts if a class file could not be loaded.
    ClassObject& forName(FieldType fieldType);

    /// Returns the class object for 'fieldDescriptor', which must be a valid field descriptor,
    /// if it has been loaded previously. Null otherwise.
    ClassObject* forNameLoaded(FieldType fieldType);

    /// Loads java classes required to boot up the VM. This a separate method and not executed as part of the
    /// constructor as it requires the VM to already be ready to execute JVM Bytecode (one that does not depend on the
    /// bootstrap classes) and to have at the very least initialized the builtin native methods of the bootstrap
    /// classes.
    /// Returns the meta class object.
    ClassObject& loadBootstrapClasses();
};

} // namespace jllvm
