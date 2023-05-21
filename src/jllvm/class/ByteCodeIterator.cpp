#include "ByteCodeIterator.hpp"

#include <llvm/Support/Alignment.h>
#include <llvm/Support/Endian.h>

std::size_t jllvm::ByteCodeIterator::currentOpSize() const
{
    switch (static_cast<OpCodes>(*m_current))
    {
        case OpCodes::AALoad:
        case OpCodes::AAStore:
        case OpCodes::AConstNull:
        case OpCodes::ALoad0:
        case OpCodes::ALoad1:
        case OpCodes::ALoad2:
        case OpCodes::ALoad3:
        case OpCodes::AReturn:
        case OpCodes::ArrayLength:
        case OpCodes::AStore0:
        case OpCodes::AStore1:
        case OpCodes::AStore2:
        case OpCodes::AStore3:
        case OpCodes::AThrow:
        case OpCodes::BALoad:
        case OpCodes::BAStore:
        case OpCodes::CALoad:
        case OpCodes::CAStore:
        case OpCodes::D2F:
        case OpCodes::D2I:
        case OpCodes::D2L:
        case OpCodes::DAdd:
        case OpCodes::DALoad:
        case OpCodes::DAStore:
        case OpCodes::DCmpG:
        case OpCodes::DCmpL:
        case OpCodes::DConst0:
        case OpCodes::DConst1:
        case OpCodes::DDiv:
        case OpCodes::DLoad0:
        case OpCodes::DLoad1:
        case OpCodes::DLoad2:
        case OpCodes::DLoad3:
        case OpCodes::DMul:
        case OpCodes::DNeg:
        case OpCodes::DRem:
        case OpCodes::DReturn:
        case OpCodes::DStore0:
        case OpCodes::DStore1:
        case OpCodes::DStore2:
        case OpCodes::DStore3:
        case OpCodes::DSub:
        case OpCodes::Dup:
        case OpCodes::DupX1:
        case OpCodes::DupX2:
        case OpCodes::Dup2:
        case OpCodes::Dup2X1:
        case OpCodes::Dup2X2:
        case OpCodes::F2D:
        case OpCodes::F2I:
        case OpCodes::F2L:
        case OpCodes::FAdd:
        case OpCodes::FALoad:
        case OpCodes::FAStore:
        case OpCodes::FCmpG:
        case OpCodes::FCmpL:
        case OpCodes::FConst0:
        case OpCodes::FConst1:
        case OpCodes::FConst2:
        case OpCodes::FDiv:
        case OpCodes::FLoad0:
        case OpCodes::FLoad1:
        case OpCodes::FLoad2:
        case OpCodes::FLoad3:
        case OpCodes::FMul:
        case OpCodes::FNeg:
        case OpCodes::FRem:
        case OpCodes::FReturn:
        case OpCodes::FStore0:
        case OpCodes::FStore1:
        case OpCodes::FStore2:
        case OpCodes::FStore3:
        case OpCodes::FSub:
        case OpCodes::I2B:
        case OpCodes::I2C:
        case OpCodes::I2D:
        case OpCodes::I2F:
        case OpCodes::I2L:
        case OpCodes::I2S:
        case OpCodes::IAdd:
        case OpCodes::IALoad:
        case OpCodes::IAnd:
        case OpCodes::IAStore:
        case OpCodes::IConstM1:
        case OpCodes::IConst0:
        case OpCodes::IConst1:
        case OpCodes::IConst2:
        case OpCodes::IConst3:
        case OpCodes::IConst4:
        case OpCodes::IConst5:
        case OpCodes::IDiv:
        case OpCodes::ILoad0:
        case OpCodes::ILoad1:
        case OpCodes::ILoad2:
        case OpCodes::ILoad3:
        case OpCodes::IMul:
        case OpCodes::INeg:
        case OpCodes::IOr:
        case OpCodes::IRem:
        case OpCodes::IReturn:
        case OpCodes::IShl:
        case OpCodes::IShr:
        case OpCodes::IStore0:
        case OpCodes::IStore1:
        case OpCodes::IStore2:
        case OpCodes::IStore3:
        case OpCodes::ISub:
        case OpCodes::IUShr:
        case OpCodes::IXor:
        case OpCodes::L2D:
        case OpCodes::L2F:
        case OpCodes::L2I:
        case OpCodes::LAdd:
        case OpCodes::LALoad:
        case OpCodes::LAnd:
        case OpCodes::LAStore:
        case OpCodes::LCmp:
        case OpCodes::LConst0:
        case OpCodes::LConst1:
        case OpCodes::LDiv:
        case OpCodes::LLoad0:
        case OpCodes::LLoad1:
        case OpCodes::LLoad2:
        case OpCodes::LLoad3:
        case OpCodes::LMul:
        case OpCodes::LNeg:
        case OpCodes::LOr:
        case OpCodes::LRem:
        case OpCodes::LReturn:
        case OpCodes::LShl:
        case OpCodes::LShr:
        case OpCodes::LStore0:
        case OpCodes::LStore1:
        case OpCodes::LStore2:
        case OpCodes::LStore3:
        case OpCodes::LSub:
        case OpCodes::LUShr:
        case OpCodes::LXor:
        case OpCodes::MonitorEnter:
        case OpCodes::MonitorExit:
        case OpCodes::Nop:
        case OpCodes::Pop:
        case OpCodes::Pop2:
        case OpCodes::Return:
        case OpCodes::SALoad:
        case OpCodes::SAStore:
        case OpCodes::Swap: return 1;
        case OpCodes::ALoad:
        case OpCodes::AStore:
        case OpCodes::BIPush:
        case OpCodes::DLoad:
        case OpCodes::DStore:
        case OpCodes::FLoad:
        case OpCodes::FStore:
        case OpCodes::ILoad:
        case OpCodes::IStore:
        case OpCodes::LDC:
        case OpCodes::LLoad:
        case OpCodes::LStore:
        case OpCodes::NewArray:
        case OpCodes::Ret: return 2;

        case OpCodes::ANewArray:
        case OpCodes::CheckCast:
        case OpCodes::GetField:
        case OpCodes::GetStatic:
        case OpCodes::Goto:
        case OpCodes::IfACmpEq:
        case OpCodes::IfACmpNe:
        case OpCodes::IfICmpEq:
        case OpCodes::IfICmpNe:
        case OpCodes::IfICmpLt:
        case OpCodes::IfICmpGe:
        case OpCodes::IfICmpGt:
        case OpCodes::IfICmpLe:
        case OpCodes::IfEq:
        case OpCodes::IfNe:
        case OpCodes::IfLt:
        case OpCodes::IfGe:
        case OpCodes::IfGt:
        case OpCodes::IfLe:
        case OpCodes::IfNonNull:
        case OpCodes::IfNull:
        case OpCodes::IInc:
        case OpCodes::InstanceOf:
        case OpCodes::InvokeSpecial:
        case OpCodes::InvokeStatic:
        case OpCodes::InvokeVirtual:
        case OpCodes::JSR:
        case OpCodes::LDCW:
        case OpCodes::LDC2W:
        case OpCodes::New:
        case OpCodes::PutField:
        case OpCodes::PutStatic:
        case OpCodes::SIPush: return 3;

        case OpCodes::GotoW:
        case OpCodes::InvokeDynamic:
        case OpCodes::InvokeInterface:
        case OpCodes::JSRw: return 5;

        case OpCodes::LookupSwitch:
        {
            std::uint64_t padding = llvm::offsetToAlignedAddr(m_current + 1, llvm::Align(4));
            const char* pairCountPtr = m_current + 5 + padding;
            std::uint32_t pairCount;
            std::memcpy(&pairCount, pairCountPtr, sizeof(std::uint32_t));
            pairCount = llvm::support::endian::byte_swap(pairCount, llvm::support::big);
            return 1 + padding + 4 + 8 * pairCount;
        }

        case OpCodes::MultiANewArray: return 4;

        case OpCodes::TableSwitch:
        {
            std::uint64_t padding = llvm::offsetToAlignedAddr(m_current + 1, llvm::Align(4));
            const char* padded = m_current + 5 + padding;
            std::uint32_t lowByte;
            std::memcpy(&lowByte, padded, sizeof(std::uint32_t));
            lowByte = llvm::support::endian::byte_swap(lowByte, llvm::support::big);
            padded += sizeof(std::uint32_t);
            std::uint32_t highByte;
            std::memcpy(&highByte, padded, sizeof(std::uint32_t));
            highByte = llvm::support::endian::byte_swap(highByte, llvm::support::big);

            return 1 + padding + 4 + 4 + 4 + (highByte - lowByte + 1) * 4;
        }
        case OpCodes::Wide: return static_cast<OpCodes>(m_current[1]) == OpCodes::IInc ? 6 : 4;
    }
    llvm_unreachable("Unknown opcode");
}
