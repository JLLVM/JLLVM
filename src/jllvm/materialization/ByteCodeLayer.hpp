#pragma once

#include <llvm/ExecutionEngine/Orc/Mangling.h>

#include <jllvm/class/ClassFile.hpp>
#include <jllvm/object/ClassObject.hpp>

namespace jllvm
{
std::string mangleMethod(llvm::StringRef className, llvm::StringRef methodName, llvm::StringRef descriptor);

std::string mangleMethod(const MethodInfo& methodInfo, const ClassFile& classFile);

/// Base class layer for any layers operating on JVM Bytecode.
class ByteCodeLayer
{
protected:
    llvm::orc::MangleAndInterner& m_interner;

public:
    explicit ByteCodeLayer(llvm::orc::MangleAndInterner& mangler) : m_interner(mangler) {}

    virtual ~ByteCodeLayer() = default;
    ByteCodeLayer(const ByteCodeLayer&) = delete;
    ByteCodeLayer& operator=(const ByteCodeLayer&) = delete;
    ByteCodeLayer(ByteCodeLayer&&) = delete;
    ByteCodeLayer& operator=(ByteCodeLayer&&) = delete;

    /// Method called by the JIT to emit the requested symbols.
    virtual void emit(std::unique_ptr<llvm::orc::MaterializationResponsibility> mr, const MethodInfo* methodInfo,
                      const ClassFile* classFile, const Method* method, const ClassObject* classObject) = 0;

    /// Adds a materialization unit for the given method and class file to 'dylib'.
    llvm::Error add(llvm::orc::JITDylib& dylib, const MethodInfo* methodInfo, const ClassFile* classFile,
                    const Method* method, const ClassObject* classObject);

    /// Returns the map of symbols provided by the method and class file.
    llvm::orc::MaterializationUnit::Interface getSymbolsProvided(const MethodInfo* methodInfo,
                                                                 const ClassFile* classFile);
};

} // namespace jllvm
