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
        bool valid;
        if (item.getName() == "value")
        {
            valid = item.getOffset() == 16 && item.getType() == byteArrayDescriptor;
        }
        else if (item.getName() == "coder")
        {
            valid = item.getOffset() == 24 && item.getType() == "B";
        }
        else if (item.getName() == "hash")
        {
            valid = item.getOffset() == 28 && item.getType() == "I";
        }
        else if (item.getName() == "hashIsZero")
        {
            valid = item.getOffset() == 32 && item.getType() == "Z";
        }
        else
        {
            llvm::report_fatal_error("Unexpected field in java.lang.String: " + item.getName());
        }
        assert(valid);
    }
#endif
}

jllvm::String* jllvm::StringInterner::createString(llvm::ArrayRef<std::uint8_t> buffer, jllvm::CompactEncoding encoding)
{
    auto* value =
        Array<std::uint8_t>::create(m_allocator, m_classLoader.forNameLoaded(byteArrayDescriptor), buffer.size());
    llvm::copy(buffer, value->begin());

    auto* string = new (m_allocator.Allocate(sizeof(String), alignof(String)))
        String(getStringClassObject(), value, static_cast<std::uint8_t>(encoding));

    m_contentToStringMap.insert({{buffer, static_cast<std::uint8_t>(encoding)}, string});

    return string;
}

jllvm::String* jllvm::StringInterner::intern(llvm::StringRef utf8String)
{
    auto [buffer, encoding] = toJavaCompactEncoding(utf8String);
    return intern(buffer, encoding);
}

jllvm::String* jllvm::StringInterner::intern(llvm::ArrayRef<std::uint8_t> buffer, jllvm::CompactEncoding encoding)
{
    auto it = m_contentToStringMap.find({buffer, static_cast<uint8_t>(encoding)});
    if (it != m_contentToStringMap.end())
    {
        return it->second;
    }
    return createString(buffer, encoding);
}
