#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/Engines/HKRebindEngine.h"
#include "../../Sources/Resolvers/HKChainedFixups.h"

#define CPU_ARM64 UINT32_C(0x0100000c)
#define CPU_SUBTYPE_ARM64E 2u
#define VM_BASE UINT64_C(0x100000000)
#define IMAGE_SIZE 0x700u
#define FILE_SIZE  0x600u
#define DATA_FILE_OFFSET 0x200u
#define SLOT_IMAGE_OFFSET 0x410u
#define FIXUPS_FILE_OFFSET 0x320u
#define FIXUPS_SIZE 85u
#define BASE_ORIGINAL UINT64_C(0x12345000)
#define REPLACEMENT UINT64_C(0x56789000)

static const uint8_t UUID[16] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static void put_u16(uint8_t *b, size_t o, uint16_t v) { memcpy(b + o, &v, 2); }
static void put_u32(uint8_t *b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); }
static void put_u64(uint8_t *b, size_t o, uint64_t v) { memcpy(b + o, &v, 8); }

static void put_segment(uint8_t *b, size_t off, const char *name,
                        uint64_t vmaddr, uint64_t vmsize,
                        uint64_t fileoff, uint64_t filesize, uint32_t initprot) {
    put_u32(b, off, HK_LC_SEGMENT_64);
    put_u32(b, off + 4, HK_SEGMENT_COMMAND_64_SIZE);
    memcpy(b + off + 8, name, strlen(name));
    put_u64(b, off + 24, vmaddr);
    put_u64(b, off + 32, vmsize);
    put_u64(b, off + 40, fileoff);
    put_u64(b, off + 48, filesize);
    put_u32(b, off + 56, initprot);
    put_u32(b, off + 60, initprot);
}

static uint64_t auth_bind(uint32_t ordinal, uint16_t diversity,
                          bool addr_div, uint8_t key, uint32_t next) {
    return ordinal | ((uint64_t)diversity << 32) |
           ((uint64_t)addr_div << 48) | ((uint64_t)key << 49) |
           ((uint64_t)next << 51) | (UINT64_C(1) << 62) |
           (UINT64_C(1) << 63);
}

static void build_header(uint8_t *b) {
    put_u32(b, 0, HK_MH_MAGIC_64);
    put_u32(b, 4, CPU_ARM64);
    put_u32(b, 8, CPU_SUBTYPE_ARM64E);
    put_u32(b, 12, 6);
    put_u32(b, 16, 5);
    put_u32(b, 20, 256);
    put_segment(b, 32, "__TEXT", VM_BASE, 0x400, 0, 0x200, 5);
    put_segment(b, 104, "__DATA", VM_BASE + 0x400, 0x100,
                DATA_FILE_OFFSET, 0x100, 3);
    put_segment(b, 176, "__LINKEDIT", VM_BASE + 0x500, 0x200,
                0x300, 0x200, 1);
    put_u32(b, 248, HK_LC_UUID);
    put_u32(b, 252, 24);
    memcpy(b + 256, UUID, sizeof(UUID));
    put_u32(b, 272, HK_LC_DYLD_CHAINED_FIXUPS);
    put_u32(b, 276, HK_LINKEDIT_DATA_CMD_SIZE);
    put_u32(b, 280, FIXUPS_FILE_OFFSET);
    put_u32(b, 284, FIXUPS_SIZE);
}

static void build_fixups(uint8_t *file) {
    uint8_t *b = file + FIXUPS_FILE_OFFSET;
    memset(b, 0, FIXUPS_SIZE);
    put_u32(b, 0, 0);
    put_u32(b, 4, 28);
    put_u32(b, 8, 72);
    put_u32(b, 12, 80);
    put_u32(b, 16, 1);
    put_u32(b, 20, HK_CHAINED_IMPORT_ADDEND);
    put_u32(b, 24, 0);
    put_u32(b, 28, 3);
    put_u32(b, 32, 0);
    put_u32(b, 36, 16);
    put_u32(b, 40, 0);
    put_u32(b, 44, 24);
    put_u16(b, 48, 0x100);
    put_u16(b, 50, HK_CHAINED_PTR_ARM64E_USERLAND24);
    put_u64(b, 52, 0x400);
    put_u16(b, 64, 1);
    put_u16(b, 66, 0x10);
    put_u32(b, 72, 1);
    put_u32(b, 76, 5);
    memcpy(b + 80, "_foo", 5);

    put_u64(file, DATA_FILE_OFFSET + 0x10,
            auth_bind(0, 0x1111, true, HK_PAC_KEY_IA, 1));
    put_u64(file, DATA_FILE_OFFSET + 0x18,
            auth_bind(0, 0x2222, false, HK_PAC_KEY_DB, 0));
}

typedef struct { unsigned calls; } writer_t;

static bool write_slot(void *opaque, uintptr_t address, uint64_t value) {
    writer_t *writer = opaque;
    writer->calls++;
    memcpy((void *)address, &value, sizeof(value));
    return true;
}

static void setup(uint8_t *live, uint8_t *file, writer_t *writer,
                  hk_rebind_target_t *target) {
    memset(live, 0, IMAGE_SIZE);
    memset(file, 0, FILE_SIZE);
    build_header(live);
    build_header(file);
    build_fixups(file);

    hk_pac_schema_t first = {
        .authenticated = true, .key = HK_PAC_KEY_IA,
        .diversity = 0x1111, .address_diversity = true,
    };
    hk_pac_schema_t second = {
        .authenticated = true, .key = HK_PAC_KEY_DB,
        .diversity = 0x2222, .address_diversity = false,
    };
    put_u64(live, SLOT_IMAGE_OFFSET,
            hk_pac_sign_slot(BASE_ORIGINAL + 5, &first,
                             (uintptr_t)live + SLOT_IMAGE_OFFSET));
    put_u64(live, SLOT_IMAGE_OFFSET + 8,
            hk_pac_sign_slot(BASE_ORIGINAL + 5, &second,
                             (uintptr_t)live + SLOT_IMAGE_OFFSET + 8));

    memset(target, 0, sizeof(*target));
    target->image_base = live;
    target->image_size = IMAGE_SIZE;
    target->slide = (uintptr_t)live - (uintptr_t)VM_BASE;
    target->file_image = file;
    target->file_image_size = FILE_SIZE;
    target->write = write_slot;
    target->write_ctx = writer;
}

int main(void) {
    uint8_t *live = aligned_alloc(64, IMAGE_SIZE);
    uint8_t *file = aligned_alloc(64, FILE_SIZE);
    assert(live && file);
    writer_t writer = {0};
    hk_rebind_target_t target;
    setup(live, file, &writer, &target);

    hk_rebind_plan_t plan;
    assert(hk_rebind_prepare(&target, "foo", HK_SYMBOL_NAME_C, &plan) ==
           HK_REBIND_OK);
    assert(plan.count == 2 && plan.originals_agree);
    assert(hk_pac_strip_code(plan.original) == BASE_ORIGINAL);
    assert(plan.sites[0].original != plan.sites[1].original);
    assert(plan.sites[0].callable_original == plan.sites[1].callable_original);
    assert(plan.sites[0].addend == 5 && plan.sites[1].addend == 5);

    uint64_t expected0 = 0, expected1 = 0;
    assert(hk_rebind_replacement_for_site(&plan.sites[0], REPLACEMENT,
                                          &expected0));
    assert(hk_rebind_replacement_for_site(&plan.sites[1], REPLACEMENT,
                                          &expected1));
    assert(expected0 != expected1);
    uint32_t written = 0;
    assert(hk_rebind_commit(&target, &plan, REPLACEMENT, NULL, &written) ==
           HK_MUTATION_COMPLETE);
    assert(written == 2 && writer.calls == 2);
    assert(hk_rebind_read_slot((uintptr_t)live + SLOT_IMAGE_OFFSET) == expected0);
    assert(hk_rebind_read_slot((uintptr_t)live + SLOT_IMAGE_OFFSET + 8) == expected1);

    setup(live, file, &writer, &target);
    file[256] ^= 1;
    writer.calls = 0;
    assert(hk_rebind_prepare(&target, "foo", HK_SYMBOL_NAME_C, &plan) ==
           HK_REBIND_MALFORMED_IMAGE);
    assert(writer.calls == 0);

    setup(live, file, &writer, &target);
    hk_pac_schema_t wrong = {
        .authenticated = true, .key = HK_PAC_KEY_IB,
        .diversity = 0x1111, .address_diversity = true,
    };
    put_u64(live, SLOT_IMAGE_OFFSET,
            hk_pac_sign_slot(BASE_ORIGINAL + 5, &wrong,
                             (uintptr_t)live + SLOT_IMAGE_OFFSET));
    writer.calls = 0;
    assert(hk_rebind_prepare(&target, "foo", HK_SYMBOL_NAME_C, &plan) ==
           HK_REBIND_PAC_MISMATCH);
    assert(writer.calls == 0);

    free(file);
    free(live);
    printf("file-backed chained PAC rebind tests passed\n");
    return 0;
}
