#pragma once

#include <llvm/Support/raw_ostream.h>

#include <jllvm/object/ClassLoader.hpp>
#include <jllvm/support/Encoding.hpp>

namespace jllvm
{
class StringInterner
{
    static constexpr auto stringDescriptor = "Ljava/lang/String;";
    static constexpr auto byteArrayDescriptor = "[B";

    llvm::DenseMap<std::pair<llvm::ArrayRef<std::uint8_t>, std::uint8_t>, String*> m_contentToStringMap;
    llvm::BumpPtrAllocator m_allocator;
    ClassLoader& m_classLoader;
    const ClassObject* m_stringClass{nullptr};

    const ClassObject* getStringClassObject();

    void checkStructure();

    String* createString(llvm::ArrayRef<std::uint8_t> buffer, jllvm::CompactEncoding encoding);

public:
    StringInterner(ClassLoader& classLoader) : m_classLoader(classLoader) {}

    String* intern(llvm::StringRef utf8String);

    String* intern(llvm::ArrayRef<std::uint8_t> buffer, jllvm::CompactEncoding encoding);
};
} // namespace jllvm
