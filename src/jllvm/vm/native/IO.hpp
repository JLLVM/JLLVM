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

namespace jllvm::io
{

class FileDescriptorModel : public ModelBase<FileDescriptorModel>
{
public:
    using Base::Base;

    struct State
    {
        InstanceFieldRef<std::uint32_t> fdField;
    };

    static void initIDs(State& state, GCRootRef<ClassObject> classObject)
    {
        state.fdField = classObject->getInstanceField<std::uint32_t>("fd", "I");
    }

    static std::uint64_t getHandle(GCRootRef<ClassObject>, std::uint32_t)
    {
        // Noop on Unix, would return handle on Windows.
        return -1;
    }

    static bool getAppend(GCRootRef<ClassObject>, std::uint32_t fd)
    {
        return isAppendMode(fd);
    }

    void close0();

    constexpr static llvm::StringLiteral className = "java/io/FileDescriptor";
    constexpr static auto methods = std::make_tuple(&FileDescriptorModel::initIDs, &FileDescriptorModel::getHandle,
                                                    &FileDescriptorModel::getAppend, &FileDescriptorModel::close0);
};

class FileOutputStreamModel : public ModelBase<FileOutputStreamModel>
{
public:
    struct State
    {
        InstanceFieldRef<Object*> descriptor;
        InstanceFieldRef<std::uint32_t> fdField;
    };

    using Base::Base;

    static void initIDs(State& state, VirtualMachine& virtualMachine, GCRootRef<ClassObject> classObject)
    {
        state.descriptor = classObject->getInstanceField<Object*>("fd", "Ljava/io/FileDescriptor;");
        state.fdField = virtualMachine.getClassLoader()
                            .forName("Ljava/io/FileDescriptor;")
                            .getInstanceField<std::uint32_t>("fd", "I");
    }

    void writeBytes(GCRootRef<Array<std::uint8_t>> bytes, std::int32_t offset, std::int32_t length, bool append)
    {
        llvm::raw_fd_ostream stream(state().fdField(state().descriptor(javaThis)), /*shouldClose=*/false);
        if (append && stream.supportsSeeking())
        {
            // TODO:
        }
        stream.write(reinterpret_cast<char*>(bytes->data() + offset), length);
    }

    constexpr static llvm::StringLiteral className = "java/io/FileOutputStream";
    constexpr static auto methods =
        std::make_tuple(&FileOutputStreamModel::initIDs, &FileOutputStreamModel::writeBytes);
};

} // namespace jllvm::io
