#include "kvm.h"

#include <string.h>

static uint32_t instruction_immediate(const struct kvm_instruction *i)
{
    return (uint32_t)i->immediate_le[0] |
           ((uint32_t)i->immediate_le[1] << 8) |
           ((uint32_t)i->immediate_le[2] << 16) |
           ((uint32_t)i->immediate_le[3] << 24);
}
static int32_t instruction_signed_immediate(const struct kvm_instruction *i)
{
    return (int32_t)instruction_immediate(i);
}
static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t read_le64(const uint8_t *p)
{
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}
int kvm_module_decode(const void *blob, size_t blob_size,
                      struct kvm_module *module)
{
    const uint8_t *bytes = (const uint8_t *)blob;
    uint32_t code_bytes, data_bytes, pages, entry;
    size_t expected;
    if (module) memset(module, 0, sizeof(*module));
    if (!bytes || !module || blob_size < KVM_MODULE_HEADER_SIZE ||
        bytes[0] != 'K' || bytes[1] != 'V' || bytes[2] != 'M' || bytes[3] != '1' ||
        read_le16(bytes + 4) != KVM_MODULE_VERSION ||
        read_le16(bytes + 6) != KVM_MODULE_HEADER_SIZE)
        return KVM_E_FORMAT;
    code_bytes = read_le32(bytes + 8);
    data_bytes = read_le32(bytes + 12);
    pages = read_le32(bytes + 16);
    entry = read_le32(bytes + 20);
    if (!code_bytes || (code_bytes % 8u) || !pages ||
        pages > SIZE_MAX / KVM_PAGE_SIZE)
        return KVM_E_FORMAT;
    if ((size_t)code_bytes > SIZE_MAX - KVM_MODULE_HEADER_SIZE ||
        (size_t)data_bytes > SIZE_MAX - KVM_MODULE_HEADER_SIZE - code_bytes)
        return KVM_E_FORMAT;
    expected = KVM_MODULE_HEADER_SIZE + (size_t)code_bytes + (size_t)data_bytes;
    if (expected != blob_size ||
        data_bytes > (size_t)pages * KVM_PAGE_SIZE ||
        entry >= code_bytes / 8u)
        return KVM_E_FORMAT;
    module->code = bytes + KVM_MODULE_HEADER_SIZE;
    module->code_size = code_bytes;
    module->data = data_bytes ? bytes + KVM_MODULE_HEADER_SIZE + code_bytes : 0;
    module->data_size = data_bytes;
    module->memory_pages = pages;
    module->entry = entry;
    module->required_capabilities = read_le64(bytes + 24);
    return KVM_OK;
}

static int registers_valid(const struct kvm_instruction *i, int dst, int a, int b)
{
    if ((dst && i->dst >= KVM_REGISTER_COUNT) ||
        (a && i->a >= KVM_REGISTER_COUNT) ||
        (b && i->b >= KVM_REGISTER_COUNT))
        return 0;
    return 1;
}

static int fields_zero(const struct kvm_instruction *i,
                       int keep_dst, int keep_a, int keep_b, int keep_imm)
{
    return (keep_dst || i->dst == 0) &&
           (keep_a || i->a == 0) &&
           (keep_b || i->b == 0) &&
           (keep_imm || instruction_immediate(i) == 0);
}

static int target_valid(const struct kvm_module *m, int32_t target)
{
    return target >= 0 && (uint32_t)target < m->code_size / sizeof(struct kvm_instruction);
}

int kvm_verify(const struct kvm_module *m, uint64_t granted_capabilities)
{
    size_t n;

    if (!m || !m->code || !m->code_size ||
        (m->code_size % sizeof(struct kvm_instruction)) != 0 ||
        m->code_size / sizeof(struct kvm_instruction) > UINT32_MAX ||
        m->entry >= m->code_size / sizeof(struct kvm_instruction) ||
        !m->memory_pages || m->memory_pages > SIZE_MAX / KVM_PAGE_SIZE ||
        (m->data_size && !m->data) ||
        m->data_size > (size_t)m->memory_pages * KVM_PAGE_SIZE)
        return KVM_E_FORMAT;
    if (m->required_capabilities & ~granted_capabilities)
        return KVM_E_CAPABILITY;
    if (sizeof(struct kvm_instruction) != 8u)
        return KVM_E_FORMAT;

    for (n = 0; n < m->code_size / sizeof(struct kvm_instruction); ++n) {
        struct kvm_instruction decoded;
        const struct kvm_instruction *i = &decoded;
        memcpy(&decoded, m->code + n * sizeof(decoded), sizeof(decoded));
        switch (i->opcode) {
        case KVM_OP_HALT:
        case KVM_OP_RET:
            if (!fields_zero(i, 0, 0, 0, 0))
                return KVM_E_FORMAT;
            break;
        case KVM_OP_CONST:
            if (!registers_valid(i, 1, 0, 0) ||
                !fields_zero(i, 1, 0, 0, 1))
                return KVM_E_REGISTER;
            break;
        case KVM_OP_MOV:
            if (!registers_valid(i, 1, 1, 0) ||
                !fields_zero(i, 1, 1, 0, 0))
                return KVM_E_REGISTER;
            break;
        case KVM_OP_ADD: case KVM_OP_SUB: case KVM_OP_MUL:
        case KVM_OP_UDIV: case KVM_OP_UMOD: case KVM_OP_AND:
        case KVM_OP_OR: case KVM_OP_XOR: case KVM_OP_SHL: case KVM_OP_SHR:
            if (!registers_valid(i, 1, 1, 1) ||
                !fields_zero(i, 1, 1, 1, 0))
                return KVM_E_REGISTER;
            break;
        case KVM_OP_LOAD8: case KVM_OP_LOAD16:
        case KVM_OP_LOAD32: case KVM_OP_LOAD64:
        case KVM_OP_STORE8: case KVM_OP_STORE16:
        case KVM_OP_STORE32: case KVM_OP_STORE64:
            if (!registers_valid(i, 1, 1, 0) ||
                !fields_zero(i, 1, 1, 0, 1))
                return KVM_E_REGISTER;
            break;
        case KVM_OP_JMP:
        case KVM_OP_CALL:
            if (!fields_zero(i, 0, 0, 0, 1))
                return KVM_E_FORMAT;
            if (!target_valid(m, instruction_signed_immediate(i)))
                return KVM_E_BRANCH;
            break;
        case KVM_OP_JZ:
        case KVM_OP_JNZ:
            if (!registers_valid(i, 0, 1, 0) ||
                !fields_zero(i, 0, 1, 0, 1))
                return KVM_E_REGISTER;
            if (!target_valid(m, instruction_signed_immediate(i)))
                return KVM_E_BRANCH;
            break;
        case KVM_OP_SYSCALL:
            if (!fields_zero(i, 0, 0, 1, 1) || instruction_signed_immediate(i) < 0 || i->b >= 64u)
                return KVM_E_FORMAT;
            if (!(m->required_capabilities & (1ull << i->b)))
                return KVM_E_CAPABILITY;
            break;
        default:
            return KVM_E_OPCODE;
        }
    }
    return KVM_OK;
}

int kvm_instance_init(struct kvm_instance *instance,
                      const struct kvm_module *module,
                      uint64_t granted_capabilities,
                      const struct kvm_allocator *allocator,
                      const struct kvm_host *host)
{
    size_t bytes, code_bytes;
    struct kvm_instruction *code_copy;
    int rc;

    if (!instance || !allocator || !allocator->allocate ||
        !allocator->release)
        return KVM_E_ARGUMENT;
    memset(instance, 0, sizeof(*instance));
    rc = kvm_verify(module, granted_capabilities);
    if (rc != KVM_OK)
        return rc;

    bytes = (size_t)module->memory_pages * KVM_PAGE_SIZE;
    code_bytes = module->code_size;
    code_copy = allocator->allocate(allocator->context, code_bytes);
    if (!code_copy)
        return KVM_E_MEMORY;
    instance->memory = allocator->allocate(allocator->context, bytes);
    if (!instance->memory) {
        allocator->release(allocator->context, code_copy, code_bytes);
        return KVM_E_MEMORY;
    }

    memcpy(code_copy, module->code, code_bytes);
    memset(instance->memory, 0, bytes);
    if (module->data_size)
        memcpy(instance->memory, module->data, module->data_size);
    instance->code = code_copy;
    instance->code_count = module->code_size / sizeof(struct kvm_instruction);
    instance->entry = module->entry;
    instance->allocator = *allocator;
    if (host)
        instance->host = *host;
    instance->memory_size = bytes;
    return KVM_OK;
}

void kvm_instance_destroy(struct kvm_instance *instance)
{
    if (!instance)
        return;
    if (instance->memory && instance->allocator.release)
        instance->allocator.release(instance->allocator.context,
                                    instance->memory, instance->memory_size);
    if (instance->code && instance->allocator.release)
        instance->allocator.release(instance->allocator.context,
                                    (void *)instance->code,
                                    instance->code_count * sizeof(struct kvm_instruction));
    memset(instance, 0, sizeof(*instance));
}

static int effective_address(const struct kvm_instance *v,
                             const struct kvm_instruction *i,
                             size_t width, size_t *address)
{
    uint64_t base = v->registers[i->a];
    int64_t offset = (int64_t)instruction_signed_immediate(i);
    uint64_t result;

    if (offset < 0) {
        uint64_t magnitude = (uint64_t)(-offset);
        if (base < magnitude)
            return 0;
        result = base - magnitude;
    } else {
        if (base > UINT64_MAX - (uint64_t)offset)
            return 0;
        result = base + (uint64_t)offset;
    }
    if (result > v->memory_size || width > v->memory_size - (size_t)result)
        return 0;
    *address = (size_t)result;
    return 1;
}

static uint64_t load_le(const uint8_t *p, size_t width)
{
    uint64_t value = 0;
    size_t n;
    for (n = 0; n < width; ++n)
        value |= (uint64_t)p[n] << (n * 8u);
    return value;
}

static void store_le(uint8_t *p, size_t width, uint64_t value)
{
    size_t n;
    for (n = 0; n < width; ++n)
        p[n] = (uint8_t)(value >> (n * 8u));
}

int kvm_run(struct kvm_instance *v, uint64_t fuel, uint64_t *exit_value)
{
    uint32_t call_stack[KVM_MAX_CALL_DEPTH];
    uint32_t depth = 0;
    uint32_t pc;

    if (!v || !v->code || !v->code_count || !v->memory || !exit_value)
        return KVM_E_ARGUMENT;
    pc = v->entry;

    while (fuel) {
        const struct kvm_instruction *i;
        uint64_t y;
        --fuel;
        if (pc >= v->code_count)
            return KVM_E_PROGRAM_COUNTER;
        i = &v->code[pc++];

        switch (i->opcode) {
        case KVM_OP_HALT:
            *exit_value = v->registers[0];
            return KVM_OK;
        case KVM_OP_CONST:
            v->registers[i->dst] = (uint64_t)(int64_t)instruction_signed_immediate(i);
            break;
        case KVM_OP_MOV:
            v->registers[i->dst] = v->registers[i->a];
            break;
        case KVM_OP_ADD:
            v->registers[i->dst] = v->registers[i->a] + v->registers[i->b];
            break;
        case KVM_OP_SUB:
            v->registers[i->dst] = v->registers[i->a] - v->registers[i->b];
            break;
        case KVM_OP_MUL:
            v->registers[i->dst] = v->registers[i->a] * v->registers[i->b];
            break;
        case KVM_OP_UDIV:
            y = v->registers[i->b];
            if (!y) return KVM_E_DIVIDE_BY_ZERO;
            v->registers[i->dst] = v->registers[i->a] / y;
            break;
        case KVM_OP_UMOD:
            y = v->registers[i->b];
            if (!y) return KVM_E_DIVIDE_BY_ZERO;
            v->registers[i->dst] = v->registers[i->a] % y;
            break;
        case KVM_OP_AND:
            v->registers[i->dst] = v->registers[i->a] & v->registers[i->b];
            break;
        case KVM_OP_OR:
            v->registers[i->dst] = v->registers[i->a] | v->registers[i->b];
            break;
        case KVM_OP_XOR:
            v->registers[i->dst] = v->registers[i->a] ^ v->registers[i->b];
            break;
        case KVM_OP_SHL:
            y = v->registers[i->b];
            if (y >= 64u) return KVM_E_SHIFT_RANGE;
            v->registers[i->dst] = v->registers[i->a] << y;
            break;
        case KVM_OP_SHR:
            y = v->registers[i->b];
            if (y >= 64u) return KVM_E_SHIFT_RANGE;
            v->registers[i->dst] = v->registers[i->a] >> y;
            break;
        case KVM_OP_LOAD8: case KVM_OP_LOAD16:
        case KVM_OP_LOAD32: case KVM_OP_LOAD64: {
            size_t width = i->opcode == KVM_OP_LOAD8 ? 1u :
                           i->opcode == KVM_OP_LOAD16 ? 2u :
                           i->opcode == KVM_OP_LOAD32 ? 4u : 8u;
            size_t address;
            if (!effective_address(v, i, width, &address))
                return KVM_E_MEMORY_BOUNDS;
            v->registers[i->dst] = load_le(v->memory + address, width);
            break;
        }
        case KVM_OP_STORE8: case KVM_OP_STORE16:
        case KVM_OP_STORE32: case KVM_OP_STORE64: {
            size_t width = i->opcode == KVM_OP_STORE8 ? 1u :
                           i->opcode == KVM_OP_STORE16 ? 2u :
                           i->opcode == KVM_OP_STORE32 ? 4u : 8u;
            size_t address;
            if (!effective_address(v, i, width, &address))
                return KVM_E_MEMORY_BOUNDS;
            store_le(v->memory + address, width, v->registers[i->dst]);
            break;
        }
        case KVM_OP_JMP:
            pc = (uint32_t)instruction_signed_immediate(i);
            break;
        case KVM_OP_JZ:
            if (!v->registers[i->a])
                pc = (uint32_t)instruction_signed_immediate(i);
            break;
        case KVM_OP_JNZ:
            if (v->registers[i->a])
                pc = (uint32_t)instruction_signed_immediate(i);
            break;
        case KVM_OP_CALL:
            if (depth == KVM_MAX_CALL_DEPTH)
                return KVM_E_CALL_STACK;
            call_stack[depth++] = pc;
            pc = (uint32_t)instruction_signed_immediate(i);
            break;
        case KVM_OP_RET:
            if (!depth)
                return KVM_E_CALL_STACK;
            pc = call_stack[--depth];
            break;
        case KVM_OP_SYSCALL: {
            uint64_t args[4], result = 0;
            if (!v->host.syscall)
                return KVM_E_HOST;
            args[0] = v->registers[0];
            args[1] = v->registers[1];
            args[2] = v->registers[2];
            args[3] = v->registers[3];
            if (v->host.syscall(v->host.context, (uint32_t)instruction_signed_immediate(i),
                                i->b, args, &result) != 0)
                return KVM_E_HOST;
            v->registers[0] = result;
            break;
        }
        default:
            return KVM_E_OPCODE;
        }
    }
    return KVM_E_FUEL;
}
