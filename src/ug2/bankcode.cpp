// Machine code inside SPEED2.EXE's audio banks (SOUND/**/*.abk, "ABKC"), linked by the bank loader 0x7212E5 and run
// from the bank tick 0x725520. Routines translated at build time (banknative.h) run natively while their bytes are
// the translated ones; others are interpreted (with sub and imm32 forms). shadow_bank=on runs
// each translated routine both ways, call by call, and compares registers, flags, stores and calls.

#include "bankcode.h"
#include "audio_mixer.h"
#include "audio_stats.h"
#include "banknative.h"

#include <lib/foreigncode.h>
#include <lib/memmap.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <recomp.h>
#include <x86.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ug2 {

void sub_7212e5_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7211ab_guest(win32::WinApplication* app, x86::CPU& cpu);

}

namespace ug2 { namespace bankcode {

// Changes whenever a bank is linked or unlinked; routine lookups and checks made in one generation are reused.
std::atomic<uint32_t> g_generation{1};

void unsupported(reg32 ip, const char* what)
{
    pf::logError("bank code: %s at 0x%08x (%02x %02x %02x)", what, ip, rc::ld8(ip), rc::ld8(ip + 1), rc::ld8(ip + 2));
    x86::guestTrap(ip, "bank code");
}

namespace {

const reg32 LoadedBanks = 0x008B4470;  // first loaded bank's list node (at bank + BankNode)
const reg32 BankNode    = 0x50;
const reg32 BankMagic   = 0x434B4241;  // "ABKC"
const int   MaxBanks    = 4096;        // list walk bound

struct Settings
{
    bool native;
    bool shadow;
    bool check;     // bank_cache_check: every cached lookup and check is redone and compared
};

const Settings& settings()
{
    return pf::cachedStatic([]
    {
        const Settings r = { NFSU_NATIVE_SETTING("bank_native"), NFSU_DEV_BOOL("shadow_bank", false),
                             NFSU_DEV_BOOL("bank_cache_check", false) };
        pf::logInfo("bank code: %u translated routines, %s%s%s, %u library functions called in place",
                    unsigned(g_translationCount), r.native ? "native" : "interpreted",
                    r.native && r.shadow ? ", shadow compare" : "", r.check ? ", cache check" : "",
                    unsigned(g_directLibraryCount));
        for (uint32_t i = 0; i < g_directLibraryCount; ++i)
            if (!mixer::libraryPure(g_directLibrary[i]))
                pf::logError("bank code: 0x%08x is called in place but is not a pure library function",
                             unsigned(g_directLibrary[i]));
        return r;
    });
}

// Bytes [begin, end) of one entry's routine in the bank at bank.
struct Range
{
    reg32 begin;
    reg32 end;
    reg32 bank;
};

// Exact routine starts win; entry layout as the loader's (0x7213CA).
bool findRoutine(reg32 address, Range& range)
{
    bool found = false;
    int banks = 0;
    for (reg32 node = rc::ld32(LoadedBanks); node && banks < MaxBanks; node = rc::ld32(node), ++banks)
    {
        const reg32 bank = node - BankNode;
        if (rc::ld32(bank) != BankMagic)
            continue;
        const reg32 count = rc::ld16(bank + 0x0A);
        reg32 entry = bank + rc::ld32(bank + 0x1C);
        for (reg32 i = 0; i < count; ++i)
        {
            const reg32 begin = rc::ld32(entry + 0x28);
            const reg32 end = rc::ld32(entry + 0x2C);
            // Ranges overlap: an entry's code can start inside the previous entry's range (SWTN_CAR_*_MB.abk).
            if (address == begin && begin < end)
            {
                range = Range{ begin, end, bank };
                return true;
            }
            if (!found && address > begin && address < end)
            {
                range = Range{ begin, end, bank };
                found = true;
            }
            entry += 0x3C + 4 * (reg32(rc::ld8(entry + 0x24)) + rc::ld8(entry + 0x27));
        }
    }
    return found;
}

const uint8_t  NoRegister = 0xff;
const unsigned WindowBytes = 12;        // compared per instruction; the longest form (81 /0, SIB, disp32) has 11

enum Op : uint8_t
{
    OpPush,         // push r32
    OpPop,          // pop r32
    OpMovStore,     // mov r/m32, r32
    OpMovLoad,      // mov r32, r/m32
    OpAdd,          // add r32, r/m32
    OpSub,          // sub r32, r/m32
    OpCmp,          // cmp r32, r/m32
    OpAddImm,       // add r/m32, imm8 / imm32
    OpCmpImm,       // cmp r/m32, imm8 / imm32
    OpStoreByte,    // mov m8, imm8
    OpImul,         // imul r32, r/m32
    OpJl,           // jl rel8
    OpJg,           // jg rel8
    OpCall,         // call rel32 (a library function patched in by the loader)
    OpRet,          // ret
};

// A decoded instruction; word holds its bytes (zero past length), mask selects them.
struct Instruction
{
    uint32_t word[WindowBytes / 4];
    uint32_t mask[WindowBytes / 4];
    reg32    start;
    uint32_t next;          // index + 1 of the instruction at start + length, 0 while not linked
    uint8_t  length;
    uint8_t  op;
    uint8_t  reg;           // ModRM reg field, or the register of push / pop
    bool     memory;        // the ModRM operand is in memory
    bool     wide;          // the WindowBytes from start lie in guest memory
    uint8_t  rm;            // the operand register when !memory
    uint8_t  base;
    uint8_t  index;
    uint8_t  scale;
    reg32    displacement;
    reg32    immediate;     // sign-extended imm8, imm32, or the jump or call target
};

// Decoded instructions of one routine; a slot per routine byte holds an index into instructions plus 1. native is
// the translation whose bytes, relocated for nativeBegin and nativeBank (expected), the routine had at its last
// check; seen holds the routine bytes of the last run that found no translation, in the bank at seenBank.
struct Routine
{
    std::vector<uint32_t>    slots;
    std::vector<Instruction> instructions;
    int                      running = 0;   // interpreters inside it (nested or preempted)
    const Translation*       native = nullptr;
    reg32                    nativeBegin = 0;
    reg32                    nativeBank = 0;
    std::vector<uint8_t>     expected;
    std::vector<uint8_t>     seen;
    reg32                    seenBank = 0;
    uint32_t                 checked = 0;   // generation in which native or seen was last confirmed
};

// Bank code runs with the SND mutex (lock 0x818088) held: no lock here.
std::unordered_map<uint64_t, std::unique_ptr<Routine>> s_routines;
const size_t MaxRoutines = 256;
const size_t MaxRoutineBytes = 0x10000;

// Cached routine of a range; null when too large.
Routine* routine(const Range& range)
{
    const reg32 size = range.end - range.begin;
    if (size > MaxRoutineBytes)
        return nullptr;
    const uint64_t key = (uint64_t(range.begin) << 32) | range.end;
    auto found = s_routines.find(key);
    if (found != s_routines.end())
        return found->second.get();
    if (s_routines.size() >= MaxRoutines)
    {
        for (auto it = s_routines.begin(); it != s_routines.end();)
            it = it->second->running ? std::next(it) : s_routines.erase(it);
    }
    std::unique_ptr<Routine> created(new Routine);
    created->slots.assign(size, 0);
    Routine* result = created.get();
    s_routines.emplace(key, std::move(created));
    return result;
}

class Running
{
public:
    explicit Running(Routine& routine) : m_routine(routine) { ++m_routine.running; }
    ~Running() { --m_routine.running; }

private:
    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;
    Routine& m_routine;
};

// Runs a routine from one call to the next, registers and flags in cpu at each.
class Interpreter
{
public:
    Interpreter(x86::CPU& cpu, const Range& range, Routine& routine, Recording* record = nullptr)
        :   m_cpu(cpu)
        ,   m_range(range)
        ,   m_routine(routine)
        ,   m_record(record)
        ,   m_running(routine)
    {
    }

    reg32 executed() const { return m_executed; }
    reg32 decoded() const { return m_decoded; }
    // Offset from the routine's start of the instruction after the current one.
    reg32 followingOffset() const
    {
        const Instruction& in = m_routine.instructions[m_current];
        return in.start + in.length - m_range.begin;
    }

    void begin(reg32 ip)
    {
        load();
        m_current = at(ip);
    }

    // True at a call (cpu.ip its target), false at ret.
    bool segment()
    {
        uint32_t current = m_current;
        for (;;)
        {
            ++m_executed;
            if (!matches(m_routine.instructions[current]))
                redecode(current);
            const Instruction& in = m_routine.instructions[current];
            switch (in.op)
            {
            case OpPush:
            {
                const reg32 value = m_r[in.reg];
                m_r[4] -= 4;
                st32(m_r[4], value);
                break;
            }
            case OpPop:
            {
                const reg32 value = rc::ld32(m_r[4]);
                m_r[4] += 4;
                m_r[in.reg] = value;
                break;
            }
            case OpMovStore:
                write32(in, address(in), m_r[in.reg]);
                break;
            case OpMovLoad:
                m_r[in.reg] = read32(in, address(in));
                break;
            case OpAdd:
                m_r[in.reg] = add(m_r[in.reg], read32(in, address(in)));
                break;
            case OpSub:
                m_r[in.reg] = subtract(m_r[in.reg], read32(in, address(in)));
                break;
            case OpCmp:
                subtract(m_r[in.reg], read32(in, address(in)));
                break;
            case OpAddImm:
            {
                const reg32 at = address(in);
                write32(in, at, add(read32(in, at), in.immediate));
                break;
            }
            case OpCmpImm:
                subtract(read32(in, address(in)), in.immediate);
                break;
            case OpStoreByte:
                st8(address(in), x86::reg8(in.immediate));
                break;
            case OpImul:
            {
                const int64_t product = int64_t(x86::sreg32(m_r[in.reg])) * x86::sreg32(read32(in, address(in)));
                const reg32 low = reg32(product);
                materialize();
                m_cpu.flags.cf = m_cpu.flags.of = (product != int64_t(x86::sreg32(low)));
                m_r[in.reg] = low;
                break;
            }
            case OpJl:
            case OpJg:
            {
                materialize();
                const bool less = m_cpu.flags.sf != m_cpu.flags.of;
                if (in.op == OpJl ? less : (!less && !m_cpu.flags.zf))
                {
                    current = at(in.immediate);
                    continue;
                }
                break;
            }
            case OpCall:
                m_r[4] -= 4;
                store();
                m_cpu.ip = in.immediate;
                m_current = current;
                return true;
            case OpRet:
                m_r[4] += 4;
                store();
                m_current = current;
                return false;
            }
            current = following(current);
        }
    }

    // After the call: registers from cpu, next instruction.
    void resume()
    {
        load();
        m_current = following(m_current);
    }

private:
    enum Pending : uint8_t
    {
        PendingNone,
        PendingAdd,
        PendingSub,
    };

    void load()
    {
        m_r[0] = m_cpu.eax;
        m_r[1] = m_cpu.ecx;
        m_r[2] = m_cpu.edx;
        m_r[3] = m_cpu.ebx;
        m_r[4] = m_cpu.esp;
        m_r[5] = m_cpu.ebp;
        m_r[6] = m_cpu.esi;
        m_r[7] = m_cpu.edi;
    }

    void store()
    {
        m_cpu.eax = m_r[0];
        m_cpu.ecx = m_r[1];
        m_cpu.edx = m_r[2];
        m_cpu.ebx = m_r[3];
        m_cpu.esp = m_r[4];
        m_cpu.ebp = m_r[5];
        m_cpu.esi = m_r[6];
        m_cpu.edi = m_r[7];
        materialize();
    }

    // Last add or sub's cf, of, zf, sf into cpu.flags.
    void materialize()
    {
        if (m_pending == PendingNone)
            return;
        const reg32 a = m_flagA, b = m_flagB, r = m_flagResult;
        if (m_pending == PendingAdd)
        {
            m_cpu.flags.cf = r < a;
            m_cpu.flags.of = ((a ^ r) & (b ^ r)) >> 31;
        }
        else
        {
            m_cpu.flags.cf = a < b;
            m_cpu.flags.of = ((a ^ b) & (a ^ r)) >> 31;
        }
        m_cpu.flags.zf = r == 0;
        m_cpu.flags.sf = r >> 31;
        m_pending = PendingNone;
    }

    // Index of the instruction at address; traps outside the routine.
    uint32_t at(reg32 address)
    {
        if (address < m_range.begin || address >= m_range.end)
        {
            store();
            unsupported(address, "jump out of the routine");
        }
        uint32_t& slot = m_routine.slots[address - m_range.begin];
        if (!slot)
        {
            Instruction in;
            decode(address, in);
            m_routine.instructions.push_back(in);
            slot = uint32_t(m_routine.instructions.size());
        }
        return slot - 1;
    }

    uint32_t following(uint32_t current)
    {
        const uint32_t next = m_routine.instructions[current].next;
        if (next)
            return next - 1;
        const Instruction& in = m_routine.instructions[current];
        const uint32_t index = at(in.start + in.length);
        m_routine.instructions[current].next = index + 1;
        return index;
    }

    bool matches(const Instruction& in) const
    {
        if (in.wide)
        {
            uint32_t word[WindowBytes / 4];
            memcpy(word, rc::host(in.start), sizeof(word));
            return (((word[0] ^ in.word[0]) & in.mask[0]) | ((word[1] ^ in.word[1]) & in.mask[1])
                    | ((word[2] ^ in.word[2]) & in.mask[2])) == 0;
        }
        return memcmp(rc::host(in.start), in.word, in.length) == 0;
    }

    void redecode(uint32_t current)
    {
        Instruction in;
        decode(m_routine.instructions[current].start, in);
        m_routine.instructions[current] = in;
    }

    // Traps on unsupported forms, as running them would.
    void decode(reg32 start, Instruction& in)
    {
        store();
        ++m_decoded;
        m_start = start;
        m_ip = start;
        in.start = start;
        in.next = 0;
        in.memory = false;
        in.reg = 0;
        in.rm = 0;
        in.base = NoRegister;
        in.index = NoRegister;
        in.scale = 0;
        in.displacement = 0;
        in.immediate = 0;
        const x86::reg8 op = fetch8();
        if (op >= 0x50 && op <= 0x5F)
        {
            in.op = op <= 0x57 ? OpPush : OpPop;
            in.reg = op & 7;
        }
        else
        {
            switch (op)
            {
            case 0x89:
            case 0x8B:
            case 0x03:
            case 0x2B:
            case 0x3B:
                in.op = op == 0x89 ? OpMovStore : op == 0x8B ? OpMovLoad : op == 0x03 ? OpAdd
                      : op == 0x2B ? OpSub : OpCmp;
                modrm(in);
                break;
            case 0x81:
            case 0x83:
                modrm(in);
                in.immediate = op == 0x81 ? fetch32() : reg32(x86::sreg32(x86::sreg8(fetch8())));
                if (in.reg != 0 && in.reg != 7)
                    unsupported(m_start, "group 1 operation");
                in.op = in.reg == 0 ? OpAddImm : OpCmpImm;
                break;
            case 0xC6:
                modrm(in);
                if (in.reg != 0)
                    unsupported(m_start, "C6 form");
                in.immediate = fetch8();
                if (!in.memory)
                    unsupported(m_start, "byte register");
                in.op = OpStoreByte;
                break;
            case 0x0F:
                if (fetch8() != 0xAF)
                    unsupported(m_start, "two-byte opcode");
                modrm(in);
                in.op = OpImul;
                break;
            case 0x7C:
            case 0x7F:
            {
                const reg32 rel = reg32(x86::sreg32(x86::sreg8(fetch8())));
                in.immediate = m_ip + rel;
                in.op = op == 0x7C ? OpJl : OpJg;
                break;
            }
            case 0xE8:
            {
                const reg32 rel = fetch32();
                in.immediate = m_ip + rel;
                in.op = OpCall;
                break;
            }
            case 0xC3:
                in.op = OpRet;
                break;
            default:
                unsupported(m_start, "instruction");
            }
        }
        in.length = uint8_t(m_ip - m_start);
        uint8_t bytes[WindowBytes] = {};
        uint8_t mask[WindowBytes] = {};
        memcpy(bytes, rc::host(m_start), in.length);
        memset(mask, 0xff, in.length);
        memcpy(in.word, bytes, sizeof(in.word));
        memcpy(in.mask, mask, sizeof(in.mask));
        in.wide = win32::MemMap::isGuestRange(m_start, WindowBytes);
    }

    x86::reg8 fetch8()
    {
        return rc::ld8(m_ip++);
    }

    reg32 fetch32()
    {
        const reg32 value = rc::ld32(m_ip);
        m_ip += 4;
        return value;
    }

    // ModRM, SIB and displacement at m_ip.
    void modrm(Instruction& in)
    {
        const x86::reg8 byte = fetch8();
        const int mod = byte >> 6;
        const int rm = byte & 7;
        in.reg = (byte >> 3) & 7;
        if (mod == 3)
        {
            in.memory = false;
            in.rm = uint8_t(rm);
            return;
        }
        in.memory = true;
        if (rm == 4)
        {
            const x86::reg8 sib = fetch8();
            const int base = sib & 7;
            const int index = (sib >> 3) & 7;
            if (base == 5 && mod == 0)
                in.displacement = fetch32();
            else
                in.base = uint8_t(base);
            if (index != 4)
            {
                in.index = uint8_t(index);
                in.scale = uint8_t(sib >> 6);
            }
        }
        else if (rm == 5 && mod == 0)
        {
            in.displacement = fetch32();
        }
        else
        {
            in.base = uint8_t(rm);
        }
        if (mod == 1)
            in.displacement += reg32(x86::sreg32(x86::sreg8(fetch8())));
        else if (mod == 2)
            in.displacement += fetch32();
    }

    reg32 address(const Instruction& in) const
    {
        reg32 address = in.base != NoRegister ? m_r[in.base] : 0;
        if (in.index != NoRegister)
            address += m_r[in.index] << in.scale;
        return address + in.displacement;
    }

    reg32 read32(const Instruction& in, reg32 address) const
    {
        return in.memory ? rc::ld32(address) : m_r[in.rm];
    }

    void write32(const Instruction& in, reg32 address, reg32 value)
    {
        if (in.memory)
            st32(address, value);
        else
            m_r[in.rm] = value;
    }

    void st32(reg32 address, reg32 value)
    {
        if (m_record)
            m_record->st32(address, value);
        else
            rc::st32(address, value);
    }

    void st8(reg32 address, x86::reg8 value)
    {
        if (m_record)
            m_record->st8(address, value);
        else
            rc::st8(address, value);
    }

    reg32 add(reg32 a, reg32 b)
    {
        const reg32 r = a + b;
        m_pending = PendingAdd;
        m_flagA = a;
        m_flagB = b;
        m_flagResult = r;
        return r;
    }

    reg32 subtract(reg32 a, reg32 b)
    {
        const reg32 r = a - b;
        m_pending = PendingSub;
        m_flagA = a;
        m_flagB = b;
        m_flagResult = r;
        return r;
    }

    x86::CPU& m_cpu;
    Range     m_range;
    Routine&  m_routine;
    Recording* m_record;
    Running   m_running;
    reg32     m_r[8];               // eax ecx edx ebx esp ebp esi edi, in cpu at calls and on return
    Pending   m_pending = PendingNone;
    reg32     m_flagA = 0;
    reg32     m_flagB = 0;
    reg32     m_flagResult = 0;
    uint32_t  m_current = 0;        // the instruction segment() continues with, or stopped at
    reg32     m_ip = 0;             // decode position
    reg32     m_start = 0;          // the instruction being decoded
    reg32     m_executed = 0;
    reg32     m_decoded = 0;
};

// Library targets that take the lock outside it: not pure and registered by name (names are fixed after start).
bool lockingLibrary(win32::WinApplication* app, reg32 target)
{
    struct Slot
    {
        reg32 target = 0;
        bool  locking = false;
    };
    static Slot s_slots[64];
    Slot& slot = s_slots[(target ^ (target >> 6)) & 63];
    if (slot.target != target)
    {
        slot.target = target;
        slot.locking = !mixer::libraryPure(target) && app->getMethodName(target);
    }
    return slot.locking;
}

// A call of cpu.ip; in the render loop without the lock, library functions that are not pure take it.
void callTarget(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 target = cpu.ip;
    const char* reason = nullptr;
    reg32 value = 0;
    if (mixer::unlocked(cpu) && lockingLibrary(app, target)
        && !mixer::libraryAudited(target, cpu, reason, value))
    {
        {
            mixer::Locked locked(app, cpu, pf::perf::SndLibrary);
            app->dynamic_call(target, cpu);
        }
        if (pf::perf::enabled())
        {
            pf::perf::audio(pf::perf::AudioLibraryLocked, 1);
            pf::perf::audioLocked(reason, value);
        }
        return;
    }
    app->dynamic_call(target, cpu);
}

void countInterpreted(const Interpreter& interpreter)
{
    if (pf::perf::enabled())
    {
        pf::perf::audio(pf::perf::AudioBankInterpreted, 1);
        pf::perf::audio(pf::perf::AudioBankInstructions, interpreter.executed());
        pf::perf::audio(pf::perf::AudioBankDecodes, interpreter.decoded());
    }
}

void interpret(win32::WinApplication* app, x86::CPU& cpu, const Range& range, Routine& routine)
{
    Interpreter interpreter(cpu, range, routine);
    interpreter.begin(cpu.ip);
    while (interpreter.segment())
    {
        if (mixstats::on())
            mixstats::bankCall(cpu, cpu.ip);
        callTarget(app, cpu);
        if (cpu.terminate)
            break;
        interpreter.resume();
    }
    countInterpreted(interpreter);
}

// ---- translations ----

// The translation's bytes as the loader leaves them for a routine at begin in the bank at bank.
void relocate(const Translation& t, reg32 begin, reg32 bank, std::vector<uint8_t>& out)
{
    out.assign(t.bytes, t.bytes + t.byteCount);
    for (uint32_t i = 0; i < t.relocationCount; ++i)
    {
        const Relocation& r = t.relocations[i];
        const reg32 value = r.kind == CallTarget ? r.value - (begin + r.offset) - 4 : r.value + bank;
        memcpy(&out[r.offset], &value, sizeof(value));
    }
}

bool spansMatch(const Translation& t, reg32 begin, const std::vector<uint8_t>& expected)
{
    for (uint32_t i = 0; i < t.spanCount; ++i)
    {
        const Span& s = t.spans[i];
        if (memcmp(rc::host(begin + s.offset), &expected[s.offset], s.length) != 0)
            return false;
    }
    return true;
}

// Content hashes of the routines logged as having no translation.
std::unordered_set<uint32_t> s_untranslated;

void logUntranslated(const Range& range)
{
    const reg32 size = range.end - range.begin;
    uint32_t hash = 2166136261u;
    for (reg32 i = 0; i < size; ++i)
        hash = (hash ^ rc::ld8(range.begin + i)) * 16777619u;
    if (s_untranslated.size() < 256 && s_untranslated.insert(hash).second)
        pf::logInfo("bank code: routine 0x%08x-0x%08x (%u bytes, FNV-1a %08x) has no translation, interpreted",
                    unsigned(range.begin), unsigned(range.end), unsigned(size), unsigned(hash));
}

// The translation of the routine's current bytes, or null.
const Translation* translationChecked(Routine& routine, const Range& range);

void cacheMismatch(const char* what, reg32 address)
{
    static unsigned s_logged = 0;
    if (s_logged < 32)
    {
        ++s_logged;
        pf::logError("bank code: cache check: %s differs at 0x%08x", what, unsigned(address));
    }
}

const Translation* translation(Routine& routine, const Range& range)
{
    const uint32_t generation = g_generation.load(std::memory_order_acquire);
    if (routine.checked == generation)
    {
        const Translation* cached = nullptr;
        bool hit = false;
        if (routine.native && routine.nativeBegin == range.begin && routine.nativeBank == range.bank)
        {
            cached = routine.native;
            hit = true;
        }
        else if (!routine.native && !routine.seen.empty() && routine.seenBank == range.bank)
            hit = true;
        if (hit)
        {
            if (settings().check && translationChecked(routine, range) != cached)
                cacheMismatch("translation", range.begin);
            return cached;
        }
    }
    const Translation* result = translationChecked(routine, range);
    routine.checked = generation;
    return result;
}

const Translation* translationChecked(Routine& routine, const Range& range)
{
    if (routine.native && routine.nativeBegin == range.begin && routine.nativeBank == range.bank
        && spansMatch(*routine.native, range.begin, routine.expected))
        return routine.native;
    routine.native = nullptr;
    const reg32 size = range.end - range.begin;
    if (!routine.seen.empty() && routine.seenBank == range.bank
        && memcmp(rc::host(range.begin), routine.seen.data(), size) == 0)
        return nullptr;
    for (uint32_t i = 0; i < g_translationCount; ++i)
    {
        const Translation& t = g_translations[i];
        if (t.length != size)
            continue;
        relocate(t, range.begin, range.bank, routine.expected);
        if (spansMatch(t, range.begin, routine.expected))
        {
            routine.native = &t;
            routine.nativeBegin = range.begin;
            routine.nativeBank = range.bank;
            routine.seen.clear();
            return &t;
        }
    }
    routine.seen.assign(rc::host(range.begin), rc::host(range.begin) + size);
    routine.seenBank = range.bank;
    logUntranslated(range);
    return nullptr;
}

void runNative(win32::WinApplication* app, x86::CPU& cpu, const Translation& t, const Range& range)
{
    Direct memory;
    uint32_t resume = 0;
    for (;;)
    {
        resume = t.run(app, cpu, memory, range.begin, range.bank, resume);
        if (resume == Returned)
            break;
        if (mixstats::on())
            mixstats::bankCall(cpu, cpu.ip);
        callTarget(app, cpu);
        if (cpu.terminate)
            break;
    }
    if (pf::perf::enabled())
        pf::perf::audio(pf::perf::AudioBankNative, 1);
}

// ---- shadow compare ----

struct Shadow
{
    uint32_t runs = 0;
    uint32_t differing = 0;
    uint32_t logged = 0;
};

Shadow s_shadow;
const uint32_t ShadowSummaryRuns = 5000;
const uint32_t ShadowDetails = 16;

// The first difference between a segment run translated (cpu native, stores of native) and interpreted (cpu, stores
// of guest), written to text; false when there is none.
bool segmentDiffers(const x86::CPU& native, const x86::CPU& guest, const Recording& nativeStores,
                    const Recording& guestStores, char* text, size_t size)
{
    static const char* const names[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    const reg32 n[] = { native.eax, native.ecx, native.edx, native.ebx, native.esp, native.ebp, native.esi,
                        native.edi };
    const reg32 g[] = { guest.eax, guest.ecx, guest.edx, guest.ebx, guest.esp, guest.ebp, guest.esi, guest.edi };
    for (int i = 0; i < 8; ++i)
    {
        if (n[i] != g[i])
        {
            snprintf(text, size, "%s native 0x%08x interpreted 0x%08x", names[i], unsigned(n[i]), unsigned(g[i]));
            return true;
        }
    }
    if (native.flags.eflags != guest.flags.eflags)
    {
        snprintf(text, size, "eflags native 0x%08x interpreted 0x%08x", unsigned(native.flags.eflags),
                 unsigned(guest.flags.eflags));
        return true;
    }
    if (native.ip != guest.ip)
    {
        snprintf(text, size, "ip native 0x%08x interpreted 0x%08x", unsigned(native.ip), unsigned(guest.ip));
        return true;
    }
    const size_t count = nativeStores.stores.size() < guestStores.stores.size() ? nativeStores.stores.size()
                                                                                 : guestStores.stores.size();
    for (size_t i = 0; i < count; ++i)
    {
        const Recording::Store& a = nativeStores.stores[i];
        const Recording::Store& b = guestStores.stores[i];
        if (a.address != b.address || a.size != b.size || a.value != b.value)
        {
            snprintf(text, size, "store %u native [0x%08x]:%u = 0x%08x interpreted [0x%08x]:%u = 0x%08x",
                     unsigned(i), unsigned(a.address), unsigned(a.size), unsigned(a.value), unsigned(b.address),
                     unsigned(b.size), unsigned(b.value));
            return true;
        }
    }
    if (nativeStores.stores.size() != guestStores.stores.size())
    {
        snprintf(text, size, "stores native %u interpreted %u", unsigned(nativeStores.stores.size()),
                 unsigned(guestStores.stores.size()));
        return true;
    }
    return false;
}

// Each segment runs translated from the state before it, its stores undone, then interpreted; the interpreted run
// is kept. After a difference the rest of the run is interpreted only.
void runShadow(win32::WinApplication* app, x86::CPU& cpu, const Translation& t, const Range& range,
               Routine& routine)
{
    Shadow& s = s_shadow;
    Recording nativeStores, guestStores;
    Interpreter interpreter(cpu, range, routine, &guestStores);
    bool compare = true;
    uint32_t resume = 0;
    char text[160] = {};
    interpreter.begin(cpu.ip);
    for (uint32_t segment = 0; ; ++segment)
    {
        bool differs = false;
        x86::CPU native;
        if (compare)
        {
            const x86::CPU before = cpu;
            resume = t.record(app, cpu, nativeStores, range.begin, range.bank, resume);
            native = cpu;
            nativeStores.undo();
            cpu = before;
        }
        guestStores.stores.clear();
        const bool called = interpreter.segment();
        if (compare)
        {
            if (called != (resume != Returned))
            {
                snprintf(text, sizeof(text), "native %s, interpreted %s", resume == Returned ? "returned" : "called",
                         called ? "called" : "returned");
                differs = true;
            }
            else if (called && t.resumeOffsets[resume] != interpreter.followingOffset())
            {
                snprintf(text, sizeof(text), "call returns to +0x%x native, +0x%x interpreted",
                         unsigned(t.resumeOffsets[resume]), unsigned(interpreter.followingOffset()));
                differs = true;
            }
            else
            {
                differs = segmentDiffers(native, cpu, nativeStores, guestStores, text, sizeof(text));
            }
            nativeStores.stores.clear();
            if (differs)
            {
                compare = false;
                ++s.differing;
                if (s.logged < ShadowDetails)
                {
                    ++s.logged;
                    pf::logInfo("shadow_bank: run %u %s at 0x%08x segment %u: %s", unsigned(s.runs + 1), t.name,
                                unsigned(range.begin), unsigned(segment), text);
                }
            }
        }
        if (!called)
            break;
        callTarget(app, cpu);
        if (cpu.terminate)
            break;
        interpreter.resume();
    }
    countInterpreted(interpreter);
    if (++s.runs % ShadowSummaryRuns == 0)
        pf::logInfo("shadow_bank: %u runs compared, %u differ", unsigned(s.runs), unsigned(s.differing));
}

int s_depth = 0;                    // bank routines running; timing_log times the outermost

struct Lookup
{
    reg32    address = 0;
    uint32_t generation = 0;
    bool     found = false;
    Range    range = { 0, 0, 0 };
};
Lookup s_lookups[256];

// findRoutine, remembered within a generation.
bool lookupRoutine(reg32 address, Range& range)
{
    const uint32_t generation = g_generation.load(std::memory_order_acquire);
    Lookup& slot = s_lookups[(address ^ (address >> 8)) & 255];
    if (slot.address == address && slot.generation == generation)
    {
        if (settings().check)
        {
            Range fresh = { 0, 0, 0 };
            const bool found = findRoutine(address, fresh);
            if (found != slot.found || (found && (fresh.begin != slot.range.begin || fresh.end != slot.range.end
                                                  || fresh.bank != slot.range.bank)))
                cacheMismatch("routine lookup", address);
        }
        range = slot.range;
        return slot.found;
    }
    slot.found = findRoutine(address, slot.range);
    slot.address = address;
    slot.generation = generation;
    range = slot.range;
    return slot.found;
}

// What resolve found, for the dynamic_call about to run.
reg32 s_resolvedAddress = 0;
Range s_resolvedRange = { 0, 0, 0 };

// Entered as a translated function: cpu.ip is the routine.
void runRoutine(win32::WinApplication* app, x86::CPU& cpu)
{
    Range range;
    const bool resolved = s_resolvedAddress == cpu.ip && cpu.ip;
    s_resolvedAddress = 0;
    if (resolved)
        range = s_resolvedRange;
    else if (!lookupRoutine(cpu.ip, range))
        unsupported(cpu.ip, "no loaded bank routine");
    const bool timed = pf::perf::enabled() && s_depth == 0;
    const uint32_t start = timed ? pf::perf::now() : 0;
    const mixstats::Scoped scope(cpu, mixstats::Bank);
    ++s_depth;
    const Settings& options = settings();
    static const bool s_library = (lib::configure(app), true);
    RC_USE(s_library);
    Routine* cached = routine(range);
    const Translation* native = cached && options.native && cpu.ip == range.begin ? translation(*cached, range)
                                                                                  : nullptr;
    if (mixstats::on())
        mixstats::bankRun(cpu, native ? native->name : nullptr, native != nullptr);
    if (native && options.shadow)
    {
        runShadow(app, cpu, *native, range, *cached);
    }
    else if (native)
    {
        runNative(app, cpu, *native, range);
    }
    else if (cached)
    {
        interpret(app, cpu, range, *cached);
    }
    else
    {
        Routine uncached;
        uncached.slots.assign(range.end - range.begin, 0);
        interpret(app, cpu, range, uncached);
    }
    --s_depth;
    if (timed)
    {
        pf::perf::audio(pf::perf::AudioBankRuns, 1);
        pf::perf::audio(pf::perf::AudioBankUs, pf::perf::now() - start);
    }
}

win32::MethodPtr resolve(reg32 address)
{
    Range range;
    if (!lookupRoutine(address, range))
        return nullptr;
    s_resolvedAddress = address;
    s_resolvedRange = range;
    return &runRoutine;
}

}

void install()
{
    win32::setForeignCodeResolver(&resolve);
}

}}

namespace ug2 { namespace native {

// Bumped on both sides, so no lookup made while the bank list changes survives it.
void sub_7212e5(win32::WinApplication* app, x86::CPU& cpu)
{
    bankcode::g_generation.fetch_add(1, std::memory_order_acq_rel);
    const uint32_t start = pf::perf::hitch() ? pf::perf::now() : 0;
    sub_7212e5_guest(app, cpu);
    if (pf::perf::hitch())
    {
        pf::perf::event(pf::perf::EventBankLoads, 1);
        pf::perf::event(pf::perf::EventBankLoadUs, pf::perf::now() - start);
    }
    bankcode::g_generation.fetch_add(1, std::memory_order_acq_rel);
}

void sub_7211ab(win32::WinApplication* app, x86::CPU& cpu)
{
    bankcode::g_generation.fetch_add(1, std::memory_order_acq_rel);
    const uint32_t start = pf::perf::hitch() ? pf::perf::now() : 0;
    sub_7211ab_guest(app, cpu);
    if (pf::perf::hitch())
    {
        pf::perf::event(pf::perf::EventBankLoads, 1);
        pf::perf::event(pf::perf::EventBankLoadUs, pf::perf::now() - start);
    }
    bankcode::g_generation.fetch_add(1, std::memory_order_acq_rel);
}

}}
