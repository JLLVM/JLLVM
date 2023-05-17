#pragma once

#include <llvm/Support/raw_ostream.h>

#include <jllvm/object/ClassLoader.hpp>
#include <jllvm/support/Encoding.hpp>

#include <map>

namespace jllvm
{
class StringInterner
{
    static constexpr auto stringDescriptor = "Ljava/lang/String;";
    static constexpr auto byteArrayDescriptor = "[B";

    std::map<std::pair<std::vector<std::uint8_t>, jllvm::CompactEncoding>, String*> m_literalToStringMap;
    llvm::BumpPtrAllocator m_allocator;
    ClassLoader& m_classLoader;
    const ClassObject* m_stringClass{nullptr};

    const ClassObject* getStringClassObject();

    void checkStructure();

    String* createString(std::pair<std::vector<std::uint8_t>, jllvm::CompactEncoding> compactEncoding);

public:
    StringInterner(ClassLoader& classLoader) : m_classLoader(classLoader) {}

    String* intern(llvm::StringRef utf8String);

    String* intern(std::pair<std::vector<std::uint8_t>, jllvm::CompactEncoding> compactEncoding);
};
} // namespace jllvm
