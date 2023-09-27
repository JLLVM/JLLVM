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

#include "Lang.hpp"

#include <llvm/Support/Endian.h>

void jllvm::lang::SystemModel::arraycopy(GCRootRef<ClassObject>, GCRootRef<Object> src, std::int32_t srcPos,
                                         GCRootRef<Object> dest, std::int32_t destPos, std::int32_t length)
{
    if (!src || !dest)
    {
        // TODO: throw NullPointerException
        llvm_unreachable("Not yet implemented");
    }
    const ClassObject* srcClass = src->getClass();
    const ClassObject* destClass = dest->getClass();

    if (!srcClass->isArray() || !destClass->isArray())
    {
        // TODO: throw ArrayStoreException
        llvm_unreachable("Not yet implemented");
    }
    const ClassObject* srcComp = srcClass->getComponentType();
    const ClassObject* destComp = destClass->getComponentType();

    if (srcComp->isPrimitive() != destComp->isPrimitive())
    {
        // TODO: throw ArrayStoreException
        llvm_unreachable("Not yet implemented");
    }

    if (srcComp->isPrimitive() && destComp->isPrimitive() && srcComp != destComp)
    {
        // TODO: throw ArrayStoreException
        llvm_unreachable("Not yet implemented");
    }

    auto srcArr = static_cast<GCRootRef<Array<>>>(src);
    auto destArr = static_cast<GCRootRef<Array<>>>(dest);

    if (srcPos < 0)
    {
        // TODO: throw IndexOutOfBoundsException
        llvm_unreachable("Not yet implemented");
    }
    if (destPos < 0)
    {
        // TODO: throw IndexOutOfBoundsException
        llvm_unreachable("Not yet implemented");
    }
    if (length < 0)
    {
        // TODO: throw IndexOutOfBoundsException
        llvm_unreachable("Not yet implemented");
    }

    if (srcPos + length > srcArr->size())
    {
        // TODO: throw IndexOutOfBoundsException
        llvm_unreachable("Not yet implemented");
    }
    if (destPos + length > destArr->size())
    {
        // TODO: throw IndexOutOfBoundsException
        llvm_unreachable("Not yet implemented");
    }

    if (srcComp->isPrimitive() || srcComp->wouldBeInstanceOf(destComp))
    {
        auto* srcBytes = reinterpret_cast<char*>(src.address()) + srcClass->getInstanceSize();
        auto* destBytes = reinterpret_cast<char*>(dest.address()) + destClass->getInstanceSize();
        std::uint32_t instanceSize = srcComp->isPrimitive() ? srcComp->getInstanceSize() : sizeof(Object*);

        std::memmove(destBytes + destPos * instanceSize, srcBytes + srcPos * instanceSize, length * instanceSize);
        return;
    }

    for (ObjectInterface* object :
         llvm::ArrayRef<ObjectInterface*>{srcArr->begin(), srcArr->end()}.slice(srcPos, length))
    {
        if (object && !object->instanceOf(destComp))
        {
            // TODO: Throw ArrayStoreException
            llvm_unreachable("Not yet implemented");
        }
        (*destArr)[destPos++] = object;
    }
}

bool jllvm::lang::StringUTF16Model::isBigEndian(GCRootRef<ClassObject>)
{
    return llvm::sys::IsBigEndianHost;
}
