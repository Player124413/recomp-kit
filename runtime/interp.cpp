// interp.cpp - see interp.h.
#include "interp.h"

#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

enum Op : uint8_t {
    NOP,
    MOV,   // dst = src
    MOV8,  // byte [mem] = imm
    LEA,
    ADD,
    OR,
    AND,
    SUB,
    XOR,
    CMP,
    TEST,
    IMUL,  // dst = dst * src, or dst = src * imm with has_imm3
    INC,
    DEC,
    PUSH,
    POP,
    CALL,  // direct: target; indirect: src
    RET,
    JMP,
    JCC,
};

// A register or memory operand. kind 0: none, 1: register, 2: memory,
// 3: immediate.
struct Operand {
    uint8_t kind = 0;
    uint8_t reg = 0;
    int8_t base = -1, index = -1;
    uint8_t scale = 1;
    int32_t disp = 0;
};

struct Ins {
    uint32_t addr = 0;
    uint8_t len = 0;
    Op op = NOP;
    uint8_t cond = 0;
    Operand dst, src;
    int32_t imm3 = 0; // IMUL r, r/m, imm
    bool has_imm3 = false;
    uint32_t target = 0; // CALL, JMP, Jcc
    bool indirect = false;
    uint16_t ret_pop = 0;
};

const uint32_t kMaxRoutine = 1u << 20;

bool readable(uint32_t a, uint32_t n) {
    return a < GUEST_SIZE && n <= GUEST_SIZE - a;
}

// ModRM (and SIB and displacement) at `p`; the operand goes to `rm` and the
// reg field to `reg`. Returns the bytes consumed, or 0.
unsigned modrm(uint32_t p, Operand &rm, uint8_t &reg) {
    if (!readable(p, 6))
        return 0;
    uint8_t m = rd8(p);
    uint8_t mod = m >> 6, r = m & 7;
    reg = (m >> 3) & 7;
    unsigned n = 1;
    if (mod == 3) {
        rm.kind = 1;
        rm.reg = r;
        return n;
    }
    rm.kind = 2;
    if (r == 4) {
        uint8_t sib = rd8(p + n++);
        uint8_t ss = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        rm.scale = (uint8_t)(1u << ss);
        rm.index = idx == 4 ? -1 : (int8_t)idx;
        if (base == 5 && mod == 0) {
            rm.base = -1;
            rm.disp = (int32_t)rd32(p + n);
            return n + 4;
        }
        rm.base = (int8_t)base;
    } else if (r == 5 && mod == 0) {
        rm.base = -1;
        rm.disp = (int32_t)rd32(p + n);
        return n + 4;
    } else {
        rm.base = (int8_t)r;
    }
    if (mod == 1) {
        rm.disp = (int8_t)rd8(p + n);
        n += 1;
    } else if (mod == 2) {
        rm.disp = (int32_t)rd32(p + n);
        n += 4;
    }
    return n;
}

Operand reg_operand(uint8_t r) {
    Operand o;
    o.kind = 1;
    o.reg = r;
    return o;
}
Operand imm_operand(int32_t v) {
    Operand o;
    o.kind = 3;
    o.disp = v;
    return o;
}

const Op kGroup1[8] = {ADD, OR, NOP, NOP, AND, SUB, XOR, CMP}; // ADC and SBB unsupported

bool decode(uint32_t a, Ins &in) {
    if (!readable(a, 16))
        return false;
    in = Ins{};
    in.addr = a;
    uint32_t p = a;
    uint8_t b = rd8(p++);
    uint8_t reg = 0;
    unsigned n;
    switch (b) {
    case 0x90:
        in.op = NOP;
        break;
    case 0x01: case 0x09: case 0x21: case 0x29: case 0x31: case 0x39: case 0x89: case 0x85:
    case 0x03: case 0x0b: case 0x23: case 0x2b: case 0x33: case 0x3b: case 0x8b: {
        Operand rm;
        if (!(n = modrm(p, rm, reg)))
            return false;
        p += n;
        const uint8_t kind = b & 0xf8;
        in.op = b == 0x85 ? TEST
                : b == 0x89 || b == 0x8b ? MOV
                : kind == 0x00 ? ADD
                : kind == 0x08 ? OR
                : kind == 0x20 ? AND
                : kind == 0x28 ? SUB
                : kind == 0x30 ? XOR
                               : CMP;
        bool to_reg = (b & 2) && b != 0x85;
        in.dst = to_reg ? reg_operand(reg) : rm;
        in.src = to_reg ? rm : reg_operand(reg);
        break;
    }
    case 0x05: case 0x0d: case 0x25: case 0x2d: case 0x35: case 0x3d:
        in.op = kGroup1[(b >> 3) & 7];
        in.dst = reg_operand(R_EAX);
        in.src = imm_operand((int32_t)rd32(p));
        p += 4;
        break;
    case 0x81: case 0x83: {
        Operand rm;
        if (!(n = modrm(p, rm, reg)))
            return false;
        p += n;
        in.op = kGroup1[reg];
        if (in.op == NOP)
            return false;
        in.dst = rm;
        if (b == 0x81) {
            in.src = imm_operand((int32_t)rd32(p));
            p += 4;
        } else {
            in.src = imm_operand((int8_t)rd8(p));
            p += 1;
        }
        break;
    }
    case 0x8d: {
        Operand rm;
        if (!(n = modrm(p, rm, reg)) || rm.kind != 2)
            return false;
        p += n;
        in.op = LEA;
        in.dst = reg_operand(reg);
        in.src = rm;
        break;
    }
    case 0xc6: case 0xc7: {
        Operand rm;
        if (!(n = modrm(p, rm, reg)) || reg != 0)
            return false;
        p += n;
        in.dst = rm;
        if (b == 0xc6) {
            if (rm.kind != 2)
                return false; // an 8-bit register
            in.op = MOV8;
            in.src = imm_operand(rd8(p));
            p += 1;
        } else {
            in.op = MOV;
            in.src = imm_operand((int32_t)rd32(p));
            p += 4;
        }
        break;
    }
    case 0x69: case 0x6b: {
        Operand rm;
        if (!(n = modrm(p, rm, reg)))
            return false;
        p += n;
        in.op = IMUL;
        in.dst = reg_operand(reg);
        in.src = rm;
        in.has_imm3 = true;
        if (b == 0x69) {
            in.imm3 = (int32_t)rd32(p);
            p += 4;
        } else {
            in.imm3 = (int8_t)rd8(p);
            p += 1;
        }
        break;
    }
    case 0x6a:
        in.op = PUSH;
        in.src = imm_operand((int8_t)rd8(p));
        p += 1;
        break;
    case 0x68:
        in.op = PUSH;
        in.src = imm_operand((int32_t)rd32(p));
        p += 4;
        break;
    case 0xc3:
        in.op = RET;
        break;
    case 0xc2:
        in.op = RET;
        in.ret_pop = rd16(p);
        p += 2;
        break;
    case 0xe8:
    case 0xe9:
        in.op = b == 0xe8 ? CALL : JMP;
        in.target = p + 4 + rd32(p);
        p += 4;
        break;
    case 0xeb:
        in.op = JMP;
        in.target = p + 1 + (int8_t)rd8(p);
        p += 1;
        break;
    case 0xff: {
        Operand rm;
        if (!(n = modrm(p, rm, reg)) || (reg != 2 && reg != 6))
            return false;
        p += n;
        if (reg == 2) {
            in.op = CALL;
            in.indirect = true;
        } else {
            in.op = PUSH;
        }
        in.src = rm;
        break;
    }
    case 0x0f: {
        uint8_t b2 = rd8(p++);
        if (b2 >= 0x80 && b2 <= 0x8f) {
            in.op = JCC;
            in.cond = b2 & 15;
            in.target = p + 4 + rd32(p);
            p += 4;
        } else if (b2 == 0xaf) {
            Operand rm;
            if (!(n = modrm(p, rm, reg)))
                return false;
            p += n;
            in.op = IMUL;
            in.dst = reg_operand(reg);
            in.src = rm;
        } else {
            return false;
        }
        break;
    }
    default:
        if (b >= 0x40 && b <= 0x4f) {
            in.op = b < 0x48 ? INC : DEC;
            in.dst = reg_operand(b & 7);
        } else if (b >= 0x50 && b <= 0x57) {
            in.op = PUSH;
            in.src = reg_operand(b & 7);
        } else if (b >= 0x58 && b <= 0x5f) {
            in.op = POP;
            in.dst = reg_operand(b & 7);
        } else if (b >= 0x70 && b <= 0x7f) {
            in.op = JCC;
            in.cond = b & 15;
            in.target = p + 1 + (int8_t)rd8(p);
            p += 1;
        } else if (b >= 0xb8 && b <= 0xbf) {
            in.op = MOV;
            in.dst = reg_operand(b & 7);
            in.src = imm_operand((int32_t)rd32(p));
            p += 4;
        } else {
            return false;
        }
    }
    in.len = (uint8_t)(p - a);
    return true;
}

struct Routine {
    uint32_t start = 0;
    std::vector<uint8_t> bytes; // the code as decoded, to notice a rewrite
    std::vector<Ins> code;
    std::unordered_map<uint32_t, uint32_t> at; // address -> index into code
};

thread_local char g_error[160];

// Decodes from `start` to the last RET or JMP that no branch passes.
std::shared_ptr<Routine> build(uint32_t start) {
    auto r = std::make_shared<Routine>();
    r->start = start;
    uint32_t a = start, furthest = start;
    for (;;) {
        Ins in;
        if (a - start >= kMaxRoutine) {
            snprintf(g_error, sizeof g_error, "no RET within %u bytes", kMaxRoutine);
            return nullptr;
        }
        if (!decode(a, in)) {
            uint32_t n = readable(a, 4) ? rd32(a) : 0;
            snprintf(g_error, sizeof g_error, "unsupported instruction at %08x (bytes %02x %02x %02x %02x)", a,
                     n & 0xff, (n >> 8) & 0xff, (n >> 16) & 0xff, n >> 24);
            return nullptr;
        }
        r->at[a] = (uint32_t)r->code.size();
        r->code.push_back(in);
        if ((in.op == JMP || in.op == JCC) && !in.indirect) {
            if (in.target < start) {
                snprintf(g_error, sizeof g_error, "branch at %08x leaves the routine for %08x", a, in.target);
                return nullptr;
            }
            if (in.target > furthest)
                furthest = in.target;
        }
        a += in.len;
        if ((in.op == RET || in.op == JMP) && a > furthest)
            break;
    }
    for (const Ins &in : r->code)
        if ((in.op == JMP || in.op == JCC) && !r->at.count(in.target)) {
            snprintf(g_error, sizeof g_error, "branch at %08x lands inside an instruction (%08x)", in.addr,
                     in.target);
            return nullptr;
        }
    r->bytes.assign(g_mem + start, g_mem + a);
    return r;
}

std::mutex g_cache_mutex;
std::unordered_map<uint32_t, std::shared_ptr<Routine>> &cache() {
    static auto *m = new std::unordered_map<uint32_t, std::shared_ptr<Routine>>();
    return *m;
}

std::shared_ptr<Routine> routine_at(uint32_t start) {
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = cache().find(start);
        if (it != cache().end()) {
            const auto &b = it->second->bytes;
            if (readable(start, (uint32_t)b.size()) && memcmp(g_mem + start, b.data(), b.size()) == 0)
                return it->second;
            cache().erase(it);
        }
    }
    auto r = build(start);
    if (r) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        cache()[start] = r;
    }
    return r;
}

// ---- execution ---------------------------------------------------------

uint32_t address_of(const X86 *c, const Operand &o) {
    uint32_t a = (uint32_t)o.disp;
    if (o.base >= 0)
        a += c->r[o.base];
    if (o.index >= 0)
        a += c->r[o.index] * o.scale;
    return a;
}

struct Fault {};

uint32_t load(const X86 *c, const Operand &o) {
    switch (o.kind) {
    case 1:
        return c->r[o.reg];
    case 3:
        return (uint32_t)o.disp;
    default: {
        uint32_t a = address_of(c, o);
        if (!readable(a, 4)) {
            snprintf(g_error, sizeof g_error, "read of %08x outside guest memory", a);
            throw Fault{};
        }
        return rd32(a);
    }
    }
}

void store(X86 *c, const Operand &o, uint32_t v) {
    if (o.kind == 1) {
        c->r[o.reg] = v;
        return;
    }
    uint32_t a = address_of(c, o);
    if (!readable(a, 4)) {
        snprintf(g_error, sizeof g_error, "write of %08x outside guest memory", a);
        throw Fault{};
    }
    wr32(a, v);
}

void push(X86 *c, uint32_t v) {
    c->r[R_ESP] -= 4;
    if (!readable(c->r[R_ESP], 4)) {
        snprintf(g_error, sizeof g_error, "stack pointer %08x outside guest memory", c->r[R_ESP]);
        throw Fault{};
    }
    wr32(c->r[R_ESP], v);
}

uint32_t pop(X86 *c) {
    if (!readable(c->r[R_ESP], 4)) {
        snprintf(g_error, sizeof g_error, "stack pointer %08x outside guest memory", c->r[R_ESP]);
        throw Fault{};
    }
    uint32_t v = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 4;
    return v;
}

uint32_t parity(uint32_t v) {
    v &= 0xff;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (~v) & 1;
}

void result_flags(X86 *c, uint32_t res) {
    c->eflags_zf = res == 0;
    c->eflags_sf = res >> 31;
    c->eflags_pf = parity(res);
}

uint32_t arith(X86 *c, Op op, uint32_t a, uint32_t b) {
    uint32_t res;
    switch (op) {
    case ADD:
        res = a + b;
        c->eflags_cf = res < a;
        c->eflags_of = ((a ^ res) & (b ^ res)) >> 31;
        c->eflags_af = ((a ^ b ^ res) >> 4) & 1;
        break;
    case SUB:
    case CMP:
        res = a - b;
        c->eflags_cf = a < b;
        c->eflags_of = ((a ^ b) & (a ^ res)) >> 31;
        c->eflags_af = ((a ^ b ^ res) >> 4) & 1;
        break;
    case AND:
    case TEST:
        res = a & b;
        c->eflags_cf = c->eflags_of = c->eflags_af = 0;
        break;
    case OR:
        res = a | b;
        c->eflags_cf = c->eflags_of = c->eflags_af = 0;
        break;
    default: // XOR
        res = a ^ b;
        c->eflags_cf = c->eflags_of = c->eflags_af = 0;
        break;
    }
    result_flags(c, res);
    return res;
}

bool condition(const X86 *c, uint8_t cc) {
    bool v;
    switch (cc >> 1) {
    case 0: v = c->eflags_of; break;
    case 1: v = c->eflags_cf; break;
    case 2: v = c->eflags_zf; break;
    case 3: v = c->eflags_cf || c->eflags_zf; break;
    case 4: v = c->eflags_sf; break;
    case 5: v = c->eflags_pf; break;
    case 6: v = c->eflags_sf != c->eflags_of; break;
    default: v = c->eflags_zf || c->eflags_sf != c->eflags_of; break;
    }
    return (cc & 1) ? !v : v;
}

// Runs from the routine's first instruction to its RET.
void run(X86 *c, const Routine &r) {
    uint32_t pc = 0;
    for (;;) {
        const Ins &in = r.code[pc];
        uint32_t next = pc + 1;
        switch (in.op) {
        case NOP:
            break;
        case MOV:
            store(c, in.dst, load(c, in.src));
            break;
        case MOV8: {
            uint32_t a = address_of(c, in.dst);
            if (!readable(a, 1)) {
                snprintf(g_error, sizeof g_error, "write of %08x outside guest memory", a);
                throw Fault{};
            }
            wr8(a, (uint8_t)in.src.disp);
            break;
        }
        case LEA:
            store(c, in.dst, address_of(c, in.src));
            break;
        case ADD:
        case OR:
        case AND:
        case SUB:
        case XOR:
            store(c, in.dst, arith(c, in.op, load(c, in.dst), load(c, in.src)));
            break;
        case CMP:
        case TEST:
            arith(c, in.op, load(c, in.dst), load(c, in.src));
            break;
        case IMUL: {
            int64_t a = (int32_t)(in.has_imm3 ? load(c, in.src) : load(c, in.dst));
            int64_t b = in.has_imm3 ? in.imm3 : (int32_t)load(c, in.src);
            int64_t full = a * b;
            uint32_t res = (uint32_t)full;
            c->eflags_cf = c->eflags_of = full != (int64_t)(int32_t)res;
            result_flags(c, res);
            store(c, in.dst, res);
            break;
        }
        case INC:
        case DEC: {
            uint32_t a = load(c, in.dst);
            uint32_t res = in.op == INC ? a + 1 : a - 1;
            c->eflags_of = in.op == INC ? res == 0x80000000u : res == 0x7fffffffu;
            c->eflags_af = ((a ^ res) >> 4) & 1;
            result_flags(c, res);
            store(c, in.dst, res);
            break;
        }
        case PUSH:
            push(c, load(c, in.src));
            break;
        case POP: {
            uint32_t v = pop(c);
            store(c, in.dst, v);
            break;
        }
        case CALL: {
            uint32_t target = in.indirect ? load(c, in.src) : in.target;
            push(c, in.addr + in.len);
            recomp_call(c, target);
            break;
        }
        case RET:
            c->eip = pop(c);
            c->r[R_ESP] += in.ret_pop;
            return;
        case JMP:
            next = r.at.at(in.target);
            break;
        case JCC:
            if (condition(c, in.cond))
                next = r.at.at(in.target);
            break;
        }
        pc = next;
        if (pc >= r.code.size()) {
            snprintf(g_error, sizeof g_error, "ran past the end of the routine at %08x", r.start);
            throw Fault{};
        }
    }
}

} // namespace

int interp_call(X86 *c, uint32_t target) {
    g_error[0] = 0;
    std::shared_ptr<Routine> r = routine_at(target);
    if (!r)
        return 0;
    const uint32_t entry_esp = c->r[R_ESP];
    try {
        run(c, *r);
    } catch (const Fault &) {
        // Part of the routine ran; carry on as if it had returned zero.
        fprintf(stderr, "[recomp] interp: routine %08x stopped: %s\n", target, g_error);
        c->r[R_EAX] = 0;
        c->r[R_ESP] = entry_esp + 4;
        c->eip = readable(entry_esp, 4) ? rd32(entry_esp) : 0;
    }
    return 1;
}

const char *interp_last_error(void) {
    return g_error;
}
