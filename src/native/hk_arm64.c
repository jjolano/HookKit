#include "hk_arm64.h"

#include <string.h>

#pragma mark - Encoders

#define A64_NOP 0xD503201Fu

// x16/x17 are IP0/IP1: call-clobbered scratch under AAPCS64, so a relocated
// prologue can never be relying on them holding anything.
#define A64_SCRATCH_JUMP 16u
#define A64_SCRATCH_LOAD 17u

static uint32_t a64_b(int64_t delta) {
    return 0x14000000u | (uint32_t)((delta >> 2) & 0x03FFFFFFu);
}

// LDR <Xt>, #delta  (64-bit load-literal)
static uint32_t a64_ldr_lit64(uint32_t rt, int64_t delta) {
    return 0x58000000u | ((uint32_t)((delta >> 2) & 0x7FFFFu) << 5) | (rt & 31u);
}

static uint32_t a64_br(uint32_t rn) {
    return 0xD61F0000u | ((rn & 31u) << 5);
}

static uint32_t a64_blr(uint32_t rn) {
    return 0xD63F0000u | ((rn & 31u) << 5);
}

static int64_t sign_extend(uint64_t value, unsigned bits) {
    uint64_t sign = 1ULL << (bits - 1);
    return (int64_t)((value ^ sign) - sign);
}

// imm19 occupies bits 23:5 in B.cond, CBZ/CBNZ and the load-literal group.
static uint32_t a64_set_imm19(uint32_t insn, int64_t delta) {
    return (insn & ~(0x7FFFFu << 5)) | ((uint32_t)((delta >> 2) & 0x7FFFFu) << 5);
}

// imm14 occupies bits 18:5 in TBZ/TBNZ.
static uint32_t a64_set_imm14(uint32_t insn, int64_t delta) {
    return (insn & ~(0x3FFFu << 5)) | ((uint32_t)((delta >> 2) & 0x3FFFu) << 5);
}

// LDR Xn, #8 / BR Xn / .quad target
static size_t emit_abs_jump(uint32_t *dst, uint64_t target) {
    // Vary the veneer scratch between IP0 (X16) and IP1 (X17) by target, so
    // the LDR/BR pair is not one fixed byte signature across every hook site.
    // Both are call-clobbered scratch, so either is a valid veneer temp -- the
    // same safety class as the fixed X16 this replaces. Deterministic by target
    // so prepare and commit encode the entry patch identically.
    // ponytail: token anti-signature; a semantic scan still sees a branch.
    uint32_t scratch = (target >> 2) & 1u ? A64_SCRATCH_LOAD : A64_SCRATCH_JUMP;
    dst[0] = a64_ldr_lit64(scratch, 8);
    dst[1] = a64_br(scratch);
    memcpy(&dst[2], &target, sizeof(target));
    return 16;
}

// Materialise a 64-bit constant into `rd` without touching any other register:
// LDR Rd, #8 / B #12 / .quad value
static size_t emit_load_const(uint32_t *dst, uint32_t rd, uint64_t value) {
    dst[0] = a64_ldr_lit64(rd, 8);
    dst[1] = a64_b(12);
    memcpy(&dst[2], &value, sizeof(value));
    return 16;
}

#pragma mark - Public helpers

bool hk_arm64_is_terminator(uint32_t insn) {
    if((insn & 0xFFFFFC1Fu) == 0xD65F0000u) {
        return true;    // RET
    }

    if((insn & 0xFFFFFC1Fu) == 0xD61F0000u) {
        return true;    // BR
    }

    if((insn & 0xFC000000u) == 0x14000000u) {
        return true;    // B (bit 31 clear distinguishes it from BL)
    }

    return false;
}

bool hk_arm64_insn_is_trap(uint32_t insn) {
    // BRK #imm16:  1101 0100 001 imm16 00000  -> 0xD4200000 | (imm16 << 5)
    // HLT #imm16:  1101 0100 010 imm16 00000  -> 0xD4400000 | (imm16 << 5)
    // UDF #imm16:  0000 0000 0000 0000 imm16  -> 0x00000000 | imm16
    // (dyld's shared-cache trap stubs use BRK #1 = 0xD4200020; __builtin_trap
    // on arm64 compiles to BRK #1 as well.)
    if((insn & 0xFFE0001Fu) == 0xD4200000u) {
        return true;    // BRK
    }

    if((insn & 0xFFE0001Fu) == 0xD4400000u) {
        return true;    // HLT
    }

    if((insn & 0xFFFF0000u) == 0u) {
        return true;    // UDF
    }

    return false;
}

// RETAA / RETAB authenticate then return; BRAA/BRAB and their Z forms are
// authenticated indirect branches. These are exactly the shapes arm64e
// micro-thunks use.
//
// Encodings (ARMv8.3-A; verified against clang's assembler and capstone):
//   BRAA  Xn, Xm  0xD71F0800 | (Rn << 5) | Rm   (key A, modifier Xm)
//   BRAB  Xn, Xm  0xD71F0C00 | (Rn << 5) | Rm   (key B)
//   BRAAZ Xn      0xD61F081F | (Rn << 5)        (key A, no modifier -- the Z
//                                                forms live in the BR space,
//                                                NOT as aliases of the pair
//                                                forms above)
//   BRABZ Xn      0xD61F0C1F | (Rn << 5)
//   RETAA         0xD65F0BFF                    (fixed)
//   RETAB         0xD65F0FFF                    (fixed)
// The authenticated CALLS (BLRAA/BLRAB/BLRAAZ/BLRABZ, 0xD73F.../0xD63F...) are
// not terminators -- they return, so clobbering one only costs a tail call.
static bool is_authenticated_terminator(uint32_t insn) {
    if(insn == 0xD65F0BFFu) {
        return true;    // RETAA (fixed encoding)
    }

    if(insn == 0xD65F0FFFu) {
        return true;    // RETAB (fixed encoding)
    }

    if((insn & 0xFFFFFC00u) == 0xD71F0800u) {
        return true;    // BRAA Xn, Xm (any Rn/Rm)
    }

    if((insn & 0xFFFFFC00u) == 0xD71F0C00u) {
        return true;    // BRAB Xn, Xm
    }

    if((insn & 0xFFFFFC1Fu) == 0xD61F081Fu) {
        return true;    // BRAAZ Xn (op2 differs from plain BR's)
    }

    if((insn & 0xFFFFFC1Fu) == 0xD61F0C1Fu) {
        return true;    // BRABZ Xn
    }

    return false;
}

bool hk_arm64_has_early_terminator(const void *win, size_t len) {
    if(!win || len < 4) {
        return false;
    }

    const uint32_t *insns = (const uint32_t *)win;
    size_t count = len / 4;

    for(size_t i = 0; i < count; i++) {
        if(hk_arm64_is_terminator(insns[i]) || is_authenticated_terminator(insns[i])) {
            return true;
        }
    }

    return false;
}

bool hk_arm64_has_aarch64_literal_load(const void *win, size_t len) {
    if(!win || len < 4) {
        return false;
    }

    const uint32_t *insns = (const uint32_t *)win;
    size_t count = len / 4;

    for(size_t i = 0; i < count; i++) {
        uint32_t insn = insns[i];

        // Load-literal group: opc(31:30) 011 V(26) 00 imm19 Rt
        if((insn & 0x3B000000u) == 0x18000000u) {
            return true;    // LDR/PRFM literal — relocation-fragile
        }

        // ADR / ADRP — PC-relative address materialisation
        if((insn & 0x1F000000u) == 0x10000000u) {
            return true;
        }
    }

    return false;
}

size_t hk_arm64_branch_size(uint64_t from, uint64_t to) {
    int64_t delta = (int64_t)to - (int64_t)from;

    // B reaches +/-128MB
    if(delta >= -(int64_t)(1LL << 27) && delta < (int64_t)(1LL << 27)) {
        return 4;
    }

    return HK_A64_MAX_BRANCH_BYTES;
}

size_t hk_arm64_emit_branch(uint32_t *buf, uint64_t buf_addr, uint64_t dest) {
    if(hk_arm64_branch_size(buf_addr, dest) == 4) {
        buf[0] = a64_b((int64_t)dest - (int64_t)buf_addr);
        return 4;
    }

    return emit_abs_jump(buf, dest);
}

#pragma mark - Relocation

// Rewrite one instruction. Returns bytes written, or 0 if unrelocatable.
// `patch_start`/`patch_end` bound the prologue bytes being overwritten: a
// branch whose target lands inside them would be retargeted back into the
// now-patched region, so it is refused (fail closed).
static size_t relocate_one(uint32_t insn, uint64_t pc, uint64_t patch_start, uint64_t patch_end, uint32_t *out) {
    // ADR / ADRP
    if((insn & 0x1F000000u) == 0x10000000u) {
        uint32_t rd = insn & 31u;
        uint64_t immlo = (insn >> 29) & 3u;
        uint64_t immhi = (insn >> 5) & 0x7FFFFu;
        int64_t imm = sign_extend((immhi << 2) | immlo, 21);
        uint64_t target;

        if(insn & 0x80000000u) {
            target = (pc & ~0xFFFULL) + ((uint64_t)imm << 12);  // ADRP
        } else {
            target = pc + (uint64_t)imm;                        // ADR
        }

        return emit_load_const(out, rd, target);
    }

    // B / BL
    if((insn & 0x7C000000u) == 0x14000000u) {
        uint64_t target = pc + (uint64_t)(sign_extend(insn & 0x03FFFFFFu, 26) << 2);

        if(target >= patch_start && target < patch_end) {
            return 0;   // lands inside the bytes being overwritten
        }

        if(!(insn & 0x80000000u)) {
            return emit_abs_jump(out, target);
        }

        // BL: the call must return here, so jump over the literal afterwards.
        // LDR X16, #12 / BLR X16 / B #12 / .quad target
        out[0] = a64_ldr_lit64(A64_SCRATCH_JUMP, 12);
        out[1] = a64_blr(A64_SCRATCH_JUMP);
        out[2] = a64_b(12);
        memcpy(&out[3], &target, sizeof(target));
        return 20;
    }

    // B.cond / CBZ / CBNZ / TBZ / TBNZ: keep the test, but retarget it at a
    // local absolute jump and fall through past it when not taken.
    //   <cond> #8
    //   B      #20
    //   <16-byte absolute jump to the original target>
    {
        bool conditional = false;
        uint64_t target = 0;
        uint32_t retargeted = 0;

        if((insn & 0xFF000010u) == 0x54000000u) {
            // B.cond
            target = pc + (uint64_t)(sign_extend((insn >> 5) & 0x7FFFFu, 19) << 2);
            retargeted = a64_set_imm19(insn, 8);
            conditional = true;
        } else if((insn & 0x7E000000u) == 0x34000000u) {
            // CBZ / CBNZ
            target = pc + (uint64_t)(sign_extend((insn >> 5) & 0x7FFFFu, 19) << 2);
            retargeted = a64_set_imm19(insn, 8);
            conditional = true;
        } else if((insn & 0x7E000000u) == 0x36000000u) {
            // TBZ / TBNZ
            target = pc + (uint64_t)(sign_extend((insn >> 5) & 0x3FFFu, 14) << 2);
            retargeted = a64_set_imm14(insn, 8);
            conditional = true;
        }

        if(conditional) {
            if(target >= patch_start && target < patch_end) {
                return 0;   // lands inside the bytes being overwritten
            }

            out[0] = retargeted;
            out[1] = a64_b(20);
            emit_abs_jump(&out[2], target);
            return 24;
        }
    }

    // Load-literal group: opc(31:30) 011 V(26) 00 imm19 Rt
    if((insn & 0x3B000000u) == 0x18000000u) {
        uint32_t opc = insn >> 30;
        uint32_t v = (insn >> 26) & 1u;
        uint32_t rt = insn & 31u;
        uint64_t target = pc + (uint64_t)(sign_extend((insn >> 5) & 0x7FFFFu, 19) << 2);
        uint32_t load;

        if(opc == 3u && v == 0u) {
            // PRFM literal is a hint; dropping it is semantically harmless.
            out[0] = A64_NOP;
            return 4;
        }

        if(!v) {
            switch(opc) {
                case 0: load = 0xB9400000u; break;  // LDR  Wt, [Xn]
                case 1: load = 0xF9400000u; break;  // LDR  Xt, [Xn]
                case 2: load = 0xB9800000u; break;  // LDRSW Xt, [Xn]
                default: return 0;
            }
        } else {
            switch(opc) {
                case 0: load = 0xBD400000u; break;  // LDR St, [Xn]
                case 1: load = 0xFD400000u; break;  // LDR Dt, [Xn]
                case 2: load = 0x3DC00000u; break;  // LDR Qt, [Xn]
                default: return 0;                  // unallocated
            }
        }

        // Clobbers x17, which AAPCS64 reserves as call-clobbered scratch, so no
        // prologue can be relying on it. The alternative would be a
        // save/restore pair around every literal load.
        emit_load_const(out, A64_SCRATCH_LOAD, target);
        out[4] = load | (A64_SCRATCH_LOAD << 5) | rt;
        return 20;
    }

    // Position-independent: copy as-is.
    out[0] = insn;
    return 4;
}

size_t hk_arm64_relocate(const uint32_t *src, uint64_t src_addr, size_t count,
                         uint32_t *dst, size_t dst_capacity) {
    size_t written = 0;

    if(!src || !dst || count == 0) {
        return 0;
    }

    for(size_t i = 0; i < count; i++) {
        if(written + HK_A64_MAX_RELOC_BYTES > dst_capacity) {
            return 0;
        }

        size_t emitted = relocate_one(src[i], src_addr + (i * 4), src_addr, src_addr + count * 4, dst + (written / 4));

        if(emitted == 0) {
            return 0;
        }

        written += emitted;
    }

    return written;
}
