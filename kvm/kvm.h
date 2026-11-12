#ifndef KIRA_KVM_H
#define KIRA_KVM_H

#include <stddef.h>
#include <stdint.h>

#define KVM_PAGE_SIZE 4096u
#define KVM_MAX_MEMORY_PAGES 4096u
#define KVM_MAX_MODULE_SIZE (16u * 1024u * 1024u)
#define KVM_REGISTER_COUNT 16u
#define KVM_MAX_CALL_DEPTH 256u
#define KVM_MODULE_HEADER_SIZE 32u
#define KVM_MODULE_VERSION 1u

/* Fixed-width, endian-independent instruction fields. Immediate values are
   interpreted as signed 32-bit values where the opcode requires it. */
struct kvm_instruction {
    uint8_t opcode;
    uint8_t dst;
    uint8_t a;
    uint8_t b;
    uint8_t immediate_le[4];
};

enum kvm_opcode {
    KVM_OP_HALT = 0,
    KVM_OP_CONST,
    KVM_OP_MOV,
    KVM_OP_ADD,
    KVM_OP_SUB,
    KVM_OP_MUL,
    KVM_OP_UDIV,
    KVM_OP_UMOD,
    KVM_OP_AND,
    KVM_OP_OR,
    KVM_OP_XOR,
    KVM_OP_SHL,
    KVM_OP_SHR,
    KVM_OP_LOAD8,
    KVM_OP_LOAD16,
    KVM_OP_LOAD32,
    KVM_OP_LOAD64,
    KVM_OP_STORE8,
    KVM_OP_STORE16,
    KVM_OP_STORE32,
    KVM_OP_STORE64,
    KVM_OP_JMP,
    KVM_OP_JZ,
    KVM_OP_JNZ,
    KVM_OP_CALL,
    KVM_OP_RET,
    KVM_OP_SYSCALL
};

enum kvm_status {
    KVM_OK = 0,
    KVM_E_ARGUMENT = -1,
    KVM_E_FORMAT = -2,
    KVM_E_CAPABILITY = -3,
    KVM_E_MEMORY = -4,
    KVM_E_OPCODE = -5,
    KVM_E_REGISTER = -6,
    KVM_E_BRANCH = -7,
    KVM_E_MEMORY_BOUNDS = -8,
    KVM_E_DIVIDE_BY_ZERO = -9,
    KVM_E_SHIFT_RANGE = -10,
    KVM_E_CALL_STACK = -11,
    KVM_E_FUEL = -12,
    KVM_E_HOST = -13,
    KVM_E_PROGRAM_COUNTER = -14
};

struct kvm_module {
    const uint8_t *code;
    size_t code_size;
    const void *data;
    size_t data_size;
    uint32_t memory_pages;
    uint32_t entry;
    uint64_t required_capabilities;
};

struct kvm_allocator {
    void *context;
    void *(*allocate)(void *context, size_t bytes);
    void (*release)(void *context, void *memory, size_t bytes);
};

struct kvm_host {
    void *context;
    /* args are registers r0-r3; result replaces r0. */
    int (*syscall)(void *context, uint32_t number, uint8_t capability,
                   const uint64_t args[4], uint64_t *result);
};

struct kvm_instance {
    const struct kvm_instruction *code;
    size_t code_count;
    uint32_t entry;
    struct kvm_allocator allocator;
    struct kvm_host host;
    uint64_t registers[KVM_REGISTER_COUNT];
    uint8_t *memory;
    size_t memory_size;
};

/* Decode the portable KVM1 container. Code instructions and header integers
   are little-endian; returned pointers refer into the caller's blob. */
int kvm_module_decode(const void *blob, size_t blob_size, struct kvm_module *module);

/* Structural verifier: validates the complete instruction stream, every
   register operand, every direct branch/call target and the granted caps. */
int kvm_verify(const struct kvm_module *module, uint64_t granted_capabilities);

/* Each instance owns a private, zero-initialized linear memory region. */
int kvm_instance_init(struct kvm_instance *instance,
                      const struct kvm_module *module,
                      uint64_t granted_capabilities,
                      const struct kvm_allocator *allocator,
                      const struct kvm_host *host);
void kvm_instance_destroy(struct kvm_instance *instance);

/* Runs until HALT or a deterministic trap. One unit of fuel is consumed per
   instruction, including branches and host calls. */
int kvm_run(struct kvm_instance *instance, uint64_t fuel,
            uint64_t *exit_value);

#endif
