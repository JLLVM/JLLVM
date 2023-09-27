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

#include <jllvm/vm/NativeImplementation.hpp>

/// Model implementations for all Java classes in a 'java/security/*' package.
namespace jllvm::security
{

class AccessControllerModel : public ModelBase<>
{
public:
    using Base::Base;

    static Object* getStackAccessControlContext(GCRootRef<ClassObject>)
    {
        // Null defined in the docs as "privileged code".
        return nullptr;
    }

    constexpr static llvm::StringLiteral className = "java/security/AccessController";
    constexpr static auto methods = std::make_tuple(&AccessControllerModel::getStackAccessControlContext);
};

} // namespace jllvm::security
