#include "Bytes.hpp"

llvm::StringRef jllvm::consumeRawString(std::size_t length, llvm::ArrayRef<char>& bytes)
{
    assert(bytes.size() >= length);
    auto result = llvm::StringRef{bytes.data(), length};
    bytes = bytes.drop_front(length);
    return result;
}
