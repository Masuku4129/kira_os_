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
    const struct kvm_module branch_module = { (const uint8_t *)bad_branch, sizeof bad_branch, 0, 0, 1, 0, 0 };
    const struct kvm_module opcode_module = { (const uint8_t *)bad_opcode, sizeof bad_opcode, 0, 0, 1, 0, 0 };
    const struct kvm_module div_module = { (const uint8_t *)div_zero, sizeof div_zero, 0, 0, 1, 0, 0 };
    const struct kvm_module oob_module = { (const uint8_t *)oob, sizeof oob, 0, 0, 1, 0, 0 };
    uint64_t result = 0;
    CHECK(kvm_verify(&branch_module, 0) == KVM_E_BRANCH);
    CHECK(kvm_verify(&opcode_module, 0) == KVM_E_OPCODE);
    CHECK(run(&div_module, 0, 20, &result) == KVM_E_DIVIDE_BY_ZERO);
    CHECK(run(&oob_module, 0, 20, &result) == KVM_E_MEMORY_BOUNDS);
    return 0;
}

int main(void)
{
    if (test_arithmetic_and_code_snapshot() ||
        test_loop_and_branches() ||
        test_memory_and_data_snapshot() ||
        test_syscall_capability() ||
        test_call_stack() ||
        test_rejects_malformed_and_traps())
        return 1;
    puts("KIRA VM v1 verifier/interpreter: PASS");
    return 0;
}
