.text
.globl asm_ref
asm_ref: movl $gbuf, %edi
         movq $gbuf, %rax
         leaq gbuf(%rip), %rcx
         ret
.data
.globl asm_ptr
asm_ptr: .quad gbuf, asm_ref
         .long gbuf
.section .note.GNU-stack,"",@progbits
