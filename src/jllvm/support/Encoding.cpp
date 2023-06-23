#include "Encoding.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/ConvertUTF.h>
#include <llvm/Support/Endian.h>

std::pair<std::vector<std::uint8_t>, jllvm::CompactEncoding> jllvm::toJavaCompactEncoding(llvm::StringRef utf8String)
{
    const llvm::UTF8* utf8Start = utf8String.bytes_begin();

    // Safe upper bound of codepoints required. We shrink the vector later.
    llvm::SmallVector<llvm::UTF32> codePoints(utf8String.size());

    llvm::UTF32* codePointsStart = codePoints.data();

    // Start of converting to UTF-32 as we need to know the values of all codepoints.
    llvm::ConvertUTF8toUTF32(&utf8Start, utf8String.bytes_end(), &codePointsStart, codePoints.end(),
                             llvm::ConversionFlags::strictConversion);
    codePoints.resize(codePointsStart - codePoints.data());

    // If they all fit within a byte use Latin1 to save on memory.
    if (llvm::all_of(codePoints, [](llvm::UTF32 codePoint) { return codePoint <= 0xFF; }))
    {
        return {std::vector<std::uint8_t>(codePoints.begin(), codePoints.end()), CompactEncoding::Latin1};
    }

    // Otherwise we convert it to UTF-16.
    const llvm::UTF32* utf32Start = codePoints.begin();

    llvm::SmallVector<llvm::UTF16> utf16(codePoints.size() * 2);

    llvm::UTF16* utf16Start = utf16.begin();

    llvm::ConvertUTF32toUTF16(&utf32Start, codePoints.end(), &utf16Start, utf16.end(),
                              llvm::ConversionFlags::strictConversion);
    utf16.resize(utf16Start - utf16.begin());

    // Convert it to a byte array now. Since Java always uses big endian we also need to switch high and low byte on
    // little endian machines.
    std::vector<std::uint8_t> result;
    for (llvm::UTF16 codePoint : utf16)
    {
        std::uint8_t temp[2];
        std::memcpy(temp, &codePoint, sizeof(llvm::UTF16));
        if constexpr (llvm::support::endianness::native != llvm::support::endianness::big)
        {
            std::swap(temp[0], temp[1]);
        }
        result.push_back(temp[0]);
        result.push_back(temp[1]);
    }
    return {std::move(result), CompactEncoding::Utf16BE};
}

std::string jllvm::fromJavaCompactEncoding(std::pair<llvm::ArrayRef<std::uint8_t>, CompactEncoding> compactEncoding)
{
    auto [buffer, encoding] = compactEncoding;
    if (encoding == CompactEncoding::Latin1)
    {
        std::string result;
        result.reserve(buffer.size());
        // First 256 codepoints in Latin1 are identical to unicode. In other words, the compact Latin1 encoding is
        // just a truncated UTF-32. Have to therefore encode these to UTF-8 explicitly.
        for (std::uint32_t unicodeCodepoint : buffer)
        {
            std::array<char, UNI_MAX_UTF8_BYTES_PER_CODE_POINT> temp{};
            char* ptr = temp.data();
            bool success = llvm::ConvertCodePointToUTF8(unicodeCodepoint, ptr);
            assert(success);
            result.append(temp.data(), ptr);
        }
        return result;
    }

    std::vector<char> utf16{buffer.begin(), buffer.end()};
    std::string string;

    if constexpr (llvm::support::endianness::native != llvm::support::endianness::big)
    {
        assert(utf16.size() % 2 == 0);
        for (auto it = utf16.begin(); it != utf16.end();)
        {
            std::swap(*it++, *it++);
        }
    }

    bool success = llvm::convertUTF16ToUTF8String(utf16, string);
    assert(success);

    return string;
}
