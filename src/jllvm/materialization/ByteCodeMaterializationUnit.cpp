#include "ByteCodeMaterializationUnit.hpp"

void jllvm::ByteCodeMaterializationUnit::materialize(std::unique_ptr<llvm::orc::MaterializationResponsibility> r)
{
    m_layer.emit(std::move(r), m_method, m_classFile);
}
