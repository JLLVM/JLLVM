#include "StringInterner.hpp"

const jllvm::ClassObject* jllvm::StringInterner::getStringClassObject()
{
    if (!m_stringClass)
    {
        m_stringClass = &m_classLoader.forName(stringDescriptor);
        checkStructure();
    }
    return m_stringClass;
}

void jllvm::StringInterner::checkStructure()
{
#ifdef NDEBUG
    for (const auto& item : m_stringClass->getFields())
    {
        if (item.isStatic())
        {
            continue;
        }
        if (item.getName() == "value")
        {
            assert(item.getOffset() == 16 && item.getType() == byteArrayDescriptor);
        }
        else if (item.getName() == "coder")
        {
            assert(item.getOffset() == 24 && item.getType() == "B");
        }
        else if (item.getName() == "hash")
        {
            assert(item.getOffset() == 28 && item.getType() == "I");
        }
        else if (item.getName() == "hashIsZero")
        {
            assert(item.getOffset() == 32 && item.getType() == "Z");
        }
        else
        {
            llvm::report_fatal_error("Unexpected field in java.lang.String: " + item.getName());
        }
    }
#endif
}

jllvm::String*
    jllvm::StringInterner::createString(std::pair<std::vector<std::uint8_t>, jllvm::CompactEncoding> compactEncoding)
{
    auto& [buffer, encoding] = compactEncoding;

    auto* value =
        Array<std::uint8_t>::create(m_allocator, m_classLoader.forNameLoaded(byteArrayDescriptor), buffer.size());
    llvm::copy(buffer, value->begin());

    auto* string = new (m_allocator.Allocate(sizeof(String), alignof(String)))
        String(getStringClassObject(), value, static_cast<std::uint8_t>(encoding));

    m_literalToStringMap.emplace(std::move(compactEncoding), string);

    return string;
}

jllvm::String* jllvm::StringInterner::intern(llvm::StringRef utf8String)
{
    return intern(toJavaCompactEncoding(utf8String));
}

jllvm::String*
    jllvm::StringInterner::intern(std::pair<std::vector<std::uint8_t>, jllvm::CompactEncoding> compactEncoding)
{
    auto it = m_literalToStringMap.find(compactEncoding);
    if (it != m_literalToStringMap.end())
    {
        return it->second;
    }
    return createString(std::move(compactEncoding));
}
