#include "../kvm/kvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "kvm_test:%d: %s\n", __LINE__, #x); return 1; } } while (0)
#define INS(op,d,a,b,imm) { (op), (d), (a), (b), { (uint8_t)((uint32_t)(imm)), (uint8_t)((uint32_t)(imm) >> 8), (uint8_t)((uint32_t)(imm) >> 16), (uint8_t)((uint32_t)(imm) >> 24) } }

static void *test_alloc(void *ctx, size_t bytes)
{
    (void)ctx;
    return malloc(bytes);
}
static void test_free(void *ctx, void *memory, size_t bytes)
{
    (void)ctx; (void)bytes;
    free(memory);
}
static int test_syscall(void *ctx, uint32_t number, uint8_t capability,
                        const uint64_t args[4], uint64_t *result)
{
    (void)ctx;
    if (number != 7u || capability != 0u)
        return -1;
    *result = args[0] + args[1] + args[2] + args[3];
    return 0;
}
static const struct kvm_allocator allocator = { 0, test_alloc, test_free };
static const struct kvm_host host = { 0, test_syscall };

static int run(const struct kvm_module *module, uint64_t granted,
               uint64_t fuel, uint64_t *result)
{
    struct kvm_instance vm;
    int rc = kvm_instance_init(&vm, module, granted, &allocator, &host);
    if (rc != KVM_OK)
        return rc;
    rc = kvm_run(&vm, fuel, result);
    kvm_instance_destroy(&vm);
    return rc;
}

static int test_arithmetic_and_code_snapshot(void)
{
    struct kvm_instruction code[] = {
        INS(KVM_OP_CONST, 1, 0, 0, 20),
        INS(KVM_OP_CONST, 2, 0, 0, 22),
        INS(KVM_OP_ADD, 0, 1, 2, 0),
        INS(KVM_OP_HALT, 0, 0, 0, 0)
    };
    struct kvm_module module = { (const uint8_t *)code, sizeof code, 0, 0, 1, 0, 0 };
    struct kvm_instance vm;
    uint64_t result = 0;
    CHECK(kvm_instance_init(&vm, &module, 0, &allocator, 0) == KVM_OK);
    code[0].opcode = KVM_OP_RET; /* Verified instance executes its private code snapshot. */
    CHECK(kvm_run(&vm, 16, &result) == KVM_OK && result == 42);
    kvm_instance_destroy(&vm);
    return 0;
}

static int test_loop_and_branches(void)
{
    const struct kvm_instruction code[] = {
        INS(KVM_OP_CONST, 0, 0, 0, 0),
        INS(KVM_OP_CONST, 1, 0, 0, 5),
        INS(KVM_OP_CONST, 2, 0, 0, 1),
        INS(KVM_OP_ADD, 0, 0, 2, 0),
        INS(KVM_OP_SUB, 1, 1, 2, 0),
        INS(KVM_OP_JNZ, 0, 1, 0, 3),
        INS(KVM_OP_HALT, 0, 0, 0, 0)
    };
    const struct kvm_module module = { (const uint8_t *)code, sizeof code, 0, 0, 1, 0, 0 };
    uint64_t result = 0;
    CHECK(run(&module, 0, 100, &result) == KVM_OK && result == 5);
    CHECK(run(&module, 0, 4, &result) == KVM_E_FUEL);
    return 0;
}

static int test_memory_and_data_snapshot(void)
{
    const struct kvm_instruction code[] = {
        INS(KVM_OP_CONST, 1, 0, 0, 32),
        INS(KVM_OP_CONST, 2, 0, 0, 0x12345678),
        INS(KVM_OP_STORE32, 2, 1, 0, 4),
        INS(KVM_OP_LOAD32, 0, 1, 0, 4),
        INS(KVM_OP_HALT, 0, 0, 0, 0)
    };
    uint8_t data[4] = { 1, 2, 3, 4 };
    struct kvm_module module = { (const uint8_t *)code, sizeof code, data, sizeof data, 1, 0, 0 };
    struct kvm_instance vm;
    uint64_t result = 0;
    CHECK(kvm_instance_init(&vm, &module, 0, &allocator, 0) == KVM_OK);
    data[0] = 99;
    CHECK(vm.memory[0] == 1);
    CHECK(kvm_run(&vm, 20, &result) == KVM_OK && result == 0x12345678u);
    kvm_instance_destroy(&vm);
    return 0;
}

static int test_syscall_capability(void)
{
    const struct kvm_instruction code[] = {
        INS(KVM_OP_CONST, 0, 0, 0, 2),
        INS(KVM_OP_CONST, 1, 0, 0, 3),
        INS(KVM_OP_CONST, 2, 0, 0, 4),
        INS(KVM_OP_CONST, 3, 0, 0, 5),
        INS(KVM_OP_SYSCALL, 0, 0, 0, 7),
        INS(KVM_OP_HALT, 0, 0, 0, 0)
    };
    struct kvm_module module = { (const uint8_t *)code, sizeof code, 0, 0, 1, 0, 1ull };
    uint64_t result = 0;
    CHECK(kvm_verify(&module, 0) == KVM_E_CAPABILITY);
    CHECK(kvm_verify(&module, 1ull) == KVM_OK);
    CHECK(run(&module, 1ull, 20, &result) == KVM_OK && result == 14);
    module.required_capabilities = 0;
    CHECK(kvm_verify(&module, UINT64_MAX) == KVM_E_CAPABILITY);
    return 0;
}

static int test_call_stack(void)
{
    const struct kvm_instruction code[] = {
        INS(KVM_OP_CONST, 1, 0, 0, 21),
        INS(KVM_OP_CALL, 0, 0, 0, 4),
        INS(KVM_OP_HALT, 0, 0, 0, 0),
        INS(KVM_OP_HALT, 0, 0, 0, 0),
        INS(KVM_OP_ADD, 0, 1, 1, 0),
        INS(KVM_OP_RET, 0, 0, 0, 0)
    };
    const struct kvm_module module = { (const uint8_t *)code, sizeof code, 0, 0, 1, 0, 0 };
    uint64_t result = 0;
    CHECK(run(&module, 0, 20, &result) == KVM_OK && result == 42);

    const struct kvm_instruction recursive[] = {
        INS(KVM_OP_CALL, 0, 0, 0, 0)
    };
    const struct kvm_module recursive_module = { (const uint8_t *)recursive, sizeof recursive, 0, 0, 1, 0, 0 };
    CHECK(run(&recursive_module, 0, 1000, &result) == KVM_E_CALL_STACK);
    return 0;
}

static int test_rejects_malformed_and_traps(void)
{
    const struct kvm_instruction bad_branch[] = {
        INS(KVM_OP_JMP, 0, 0, 0, 9)
    };
    const struct kvm_instruction bad_opcode[] = {
        INS(255, 0, 0, 0, 0)
    };
    const struct kvm_instruction div_zero[] = {
        INS(KVM_OP_CONST, 1, 0, 0, 1),
        INS(KVM_OP_CONST, 2, 0, 0, 0),
        INS(KVM_OP_UDIV, 0, 1, 2, 0),
        INS(KVM_OP_HALT, 0, 0, 0, 0)
    };
    const struct kvm_instruction oob[] = {
        INS(KVM_OP_CONST, 1, 0, 0, 4096),
        INS(KVM_OP_LOAD8, 0, 1, 0, 0),
        INS(KVM_OP_HALT, 0, 0, 0, 0)
    };
    const struct kvm_instruction bad_shift[] = {
        INS(KVM_OP_CONST, 1, 0, 0, 64),
        INS(KVM_OP_CONST, 2, 0, 0, 1),
        INS(KVM_OP_SHL, 0, 2, 1, 0),
        INS(KVM_OP_HALT, 0, 0, 0, 0)
    };
    const struct kvm_instruction bad_register[] = {
        INS(KVM_OP_MOV, 16, 0, 0, 0)
    };
    const struct kvm_module branch_module = { (const uint8_t *)bad_branch, sizeof bad_branch, 0, 0, 1, 0, 0 };
    const struct kvm_module opcode_module = { (const uint8_t *)bad_opcode, sizeof bad_opcode, 0, 0, 1, 0, 0 };
    const struct kvm_module div_module = { (const uint8_t *)div_zero, sizeof div_zero, 0, 0, 1, 0, 0 };
    const struct kvm_module oob_module = { (const uint8_t *)oob, sizeof oob, 0, 0, 1, 0, 0 };
    const struct kvm_module shift_module = { (const uint8_t *)bad_shift, sizeof bad_shift, 0, 0, 1, 0, 0 };
    const struct kvm_module register_module = { (const uint8_t *)bad_register, sizeof bad_register, 0, 0, 1, 0, 0 };
    uint64_t result = 0;
    CHECK(kvm_verify(&branch_module, 0) == KVM_E_BRANCH);
    CHECK(kvm_verify(&opcode_module, 0) == KVM_E_OPCODE);
    CHECK(kvm_verify(&register_module, 0) == KVM_E_REGISTER);
    CHECK(run(&div_module, 0, 20, &result) == KVM_E_DIVIDE_BY_ZERO);
    CHECK(run(&oob_module, 0, 20, &result) == KVM_E_MEMORY_BOUNDS);
    CHECK(run(&shift_module, 0, 20, &result) == KVM_E_SHIFT_RANGE);
    return 0;
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}
static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}
static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value); put32(p + 4, (uint32_t)(value >> 32));
}
struct alloc_probe {
    unsigned calls;
    unsigned releases;
    unsigned fail_at;
};
static void *probe_alloc(void *context, size_t bytes)
{
    struct alloc_probe *probe = (struct alloc_probe *)context;
    ++probe->calls;
    if (probe->fail_at && probe->calls == probe->fail_at)
        return 0;
    return malloc(bytes);
}
static void probe_free(void *context, void *memory, size_t bytes)
{
    struct alloc_probe *probe = (struct alloc_probe *)context;
    (void)bytes;
    ++probe->releases;
    free(memory);
}
static int test_allocator_failure_cleanup(void)
{
    const struct kvm_instruction code[] = { INS(KVM_OP_HALT, 0, 0, 0, 0) };
    const struct kvm_module module = { (const uint8_t *)code, sizeof code, 0, 0, 1, 0, 0 };
    struct alloc_probe probe = { 0, 0, 1 };
    struct kvm_allocator tracked = { &probe, probe_alloc, probe_free };
    struct kvm_instance vm;

    CHECK(kvm_instance_init(&vm, &module, 0, &tracked, 0) == KVM_E_MEMORY);
    CHECK(probe.calls == 1 && probe.releases == 0);

    /* A verifier rejection after the code snapshot allocation must release it
       and must not proceed to allocate guest linear memory. */
    {
        const struct kvm_instruction invalid[] = { INS(255, 0, 0, 0, 0) };
        const struct kvm_module bad = {
            (const uint8_t *)invalid, sizeof invalid, 0, 0, 1, 0, 0
        };
        probe.calls = 0; probe.releases = 0; probe.fail_at = 0;
        CHECK(kvm_instance_init(&vm, &bad, 0, &tracked, 0) == KVM_E_OPCODE);
        CHECK(probe.calls == 1 && probe.releases == 1);
    }

    probe.calls = 0; probe.releases = 0; probe.fail_at = 2;
    CHECK(kvm_instance_init(&vm, &module, 0, &tracked, 0) == KVM_E_MEMORY);
    CHECK(probe.calls == 2 && probe.releases == 1);

    probe.calls = 0; probe.releases = 0; probe.fail_at = 0;
    CHECK(kvm_instance_init(&vm, &module, 0, &tracked, 0) == KVM_OK);
    CHECK(probe.calls == 2 && probe.releases == 0);
    kvm_instance_destroy(&vm);
    CHECK(probe.releases == 2);
    return 0;
}

static int test_resource_limits(void)
{
    const struct kvm_instruction code[] = { INS(KVM_OP_HALT, 0, 0, 0, 0) };
    struct kvm_module module = { (const uint8_t *)code, sizeof code, 0, 0, 1, 0, 0 };
    CHECK(kvm_verify(&module, 0) == KVM_OK);
    module.memory_pages = KVM_MAX_MEMORY_PAGES + 1u;
    CHECK(kvm_verify(&module, 0) == KVM_E_FORMAT);
    module.memory_pages = 1;
    module.code_size = KVM_MAX_MODULE_SIZE;
    CHECK(kvm_verify(&module, 0) == KVM_E_FORMAT);
    module.code_size = sizeof code;
    module.memory_pages = 0;
    CHECK(kvm_verify(&module, 0) == KVM_E_FORMAT);
    return 0;
}

static int test_portable_container(void)
{
    const struct kvm_instruction code[] = {
        INS(KVM_OP_CONST, 0, 0, 0, 73),
        INS(KVM_OP_HALT, 0, 0, 0, 0)
    };
    const uint8_t data[] = { 9, 8, 7, 6 };
    uint8_t blob[KVM_MODULE_HEADER_SIZE + sizeof code + sizeof data];
    struct kvm_module module;
    uint64_t result = 0;

    memset(blob, 0, sizeof blob);
    memcpy(blob, "KVP0", 4);
    put16(blob + 4, KVM_MODULE_VERSION);
    put16(blob + 6, KVM_MODULE_HEADER_SIZE);
    put32(blob + 8, (uint32_t)sizeof code);
    put32(blob + 12, (uint32_t)sizeof data);
    put32(blob + 16, 1);
    put32(blob + 20, 0);
    put64(blob + 24, 0);
    memcpy(blob + KVM_MODULE_HEADER_SIZE, code, sizeof code);
    memcpy(blob + KVM_MODULE_HEADER_SIZE + sizeof code, data, sizeof data);

    CHECK(kvm_module_decode(blob, sizeof blob, &module) == KVM_OK);
    CHECK(kvm_verify(&module, 0) == KVM_OK);
    memcpy(blob, "KVM1", 4);
    CHECK(kvm_module_decode(blob, sizeof blob, &module) == KVM_E_FORMAT);
    memcpy(blob, "KVP0", 4);
    /* A rejected decode clears the output descriptor; restore it before use. */
    CHECK(kvm_module_decode(blob, sizeof blob, &module) == KVM_OK);
    CHECK(run(&module, 0, 10, &result) == KVM_OK && result == 73);
    CHECK(kvm_module_decode(blob, sizeof blob - 1u, &module) == KVM_E_FORMAT);

    put16(blob + 4, 2);
    CHECK(kvm_module_decode(blob, sizeof blob, &module) == KVM_E_FORMAT);
    put16(blob + 4, KVM_MODULE_VERSION);
    put32(blob + 8, 7u);
    CHECK(kvm_module_decode(blob, sizeof blob, &module) == KVM_E_FORMAT);
    put32(blob + 8, (uint32_t)sizeof code);
    put32(blob + 12, KVM_PAGE_SIZE + 1u);
    CHECK(kvm_module_decode(blob, sizeof blob, &module) == KVM_E_FORMAT);
    put32(blob + 12, (uint32_t)sizeof data);
    put32(blob + 20, 2u);
    CHECK(kvm_module_decode(blob, sizeof blob, &module) == KVM_E_FORMAT);
    put32(blob + 20, 0u);
    put32(blob + 16, KVM_MAX_MEMORY_PAGES + 1u);
    CHECK(kvm_module_decode(blob, sizeof blob, &module) == KVM_E_FORMAT);
    put32(blob + 16, 1u);
    put32(blob + 8, 0xfffffff8u); /* length arithmetic must fail closed on 32-bit too */
    CHECK(kvm_module_decode(blob, sizeof blob, &module) == KVM_E_FORMAT);
    put32(blob + 8, (uint32_t)sizeof code);
    blob[0] = 'X';
    CHECK(kvm_module_decode(blob, sizeof blob, &module) == KVM_E_FORMAT);
    return 0;
}

int main(void)
{
    if (test_arithmetic_and_code_snapshot() ||
        test_loop_and_branches() ||
        test_memory_and_data_snapshot() ||
        test_syscall_capability() ||
        test_call_stack() ||
        test_rejects_malformed_and_traps() ||
        test_resource_limits() ||
        test_allocator_failure_cleanup() ||
        test_portable_container())
        return 1;
    puts("KIRA VM KVP0 prototype verifier/interpreter: PASS");
    return 0;
}
