#pragma once

#include <jllvm/vm/NativeImplementation.hpp>

namespace jllvm::io
{
struct FileDescriptor : ObjectInterface
{
    ObjectHeader header;

    std::uint32_t fd{};
    std::uint64_t handle{};
    Object* parent{};
    Object* otherParents{};
    bool closed{};
};

static_assert(std::is_standard_layout_v<FileDescriptor>);

class FileDescriptorModel : public ModelBase<FileDescriptor>
{
public:
    using Base::Base;

    static void initIDs(VirtualMachine&, GCRootRef<ClassObject>)
    {
        // Noop in our implementation.
    }

    static std::uint64_t getHandle(VirtualMachine&, GCRootRef<ClassObject>, std::uint32_t)
    {
        // Noop on Unix, would return handle on Windows.
        return -1;
    }

    static bool getAppend(VirtualMachine&, GCRootRef<ClassObject>, std::uint32_t fd)
    {
        return isAppendMode(fd);
    }

    void close0();

    constexpr static llvm::StringLiteral className = "java/io/FileDescriptor";
    constexpr static auto methods = std::make_tuple(&FileDescriptorModel::initIDs, &FileDescriptorModel::getHandle,
                                                    &FileDescriptorModel::getAppend, &FileDescriptorModel::close0);
};

} // namespace jllvm::io
