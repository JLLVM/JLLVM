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

    std::map<llvm::StringRef, String*> m_literalToStringMap;
    llvm::BumpPtrAllocator m_stringAllocator;
    llvm::BumpPtrAllocator m_valueAllocator;
    ClassLoader& m_classLoader;
    const ClassObject* m_stringClass{nullptr};

    const ClassObject* getStringClassObject();

    void checkStructure();

    String* createString(llvm::StringRef utf8String);

public:
    StringInterner(ClassLoader& classLoader) : m_classLoader(classLoader) {}

    String* intern(llvm::StringRef utf8String);
};
} // namespace jllvm
