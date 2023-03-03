#pragma once

#include <llvm/ExecutionEngine/Orc/Core.h>

#include "ByteCodeLayer.hpp"

namespace jllvm
{

/// Materialization unit to add a JVM Byte code method to the JITLink graph and materializing it once required.
class ByteCodeMaterializationUnit : public llvm::orc::MaterializationUnit
{
    jllvm::ByteCodeLayer& m_layer;
    const jllvm::MethodInfo* m_method;
    const jllvm::ClassFile* m_classFile;

public:
    /// Creates a materialization unit for the method 'methodInfo' of the class 'classFile'.
    /// Compilation is done using 'layer'.
    ByteCodeMaterializationUnit(jllvm::ByteCodeLayer& layer, const jllvm::MethodInfo* methodInfo,
                                const jllvm::ClassFile* classFile)
        : llvm::orc::MaterializationUnit(layer.getSymbolsProvided(methodInfo, classFile)),
          m_layer(layer),
          m_method(methodInfo),
          m_classFile(classFile)
    {
    }

    llvm::StringRef getName() const override
    {
        return "ByteCodeMaterializationUnit";
    }

    void materialize(std::unique_ptr<llvm::orc::MaterializationResponsibility> r) override;

private:
    void discard(const llvm::orc::JITDylib&, const llvm::orc::SymbolStringPtr&) override
    {
        llvm_unreachable("Should not be possible");
    }
};

} // namespace jllvm
