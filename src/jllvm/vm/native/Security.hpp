
#pragma once

#include <jllvm/vm/NativeImplementation.hpp>

/// Model implementations for all Java classes in a 'java/security/*' package.
namespace jllvm::security
{

class AccessControllerModel : public ModelBase<>
{
public:
    using Base::Base;

    static Object* getStackAccessControlContext(VirtualMachine&, GCRootRef<ClassObject>)
    {
        // Null defined in the docs as "privileged code".
        return nullptr;
    }

    constexpr static llvm::StringLiteral className = "java/security/AccessController";
    constexpr static auto methods = std::make_tuple(&AccessControllerModel::getStackAccessControlContext);
};

} // namespace jllvm::security
