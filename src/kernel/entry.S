// The kernel's entry point: a stack, and nothing else.
//
// docs/spec/04-ee-kernel.md EE-4 fixes this address -- the vector page occupies
// the `0x1000` bytes below it, so the entry is the first thing after. The reset
// path jumps here with no stack of ours, which is the only reason this is
// assembly: a compiled function needs the stack to already exist. What the
// kernel then does is `entry.cpp`.

    .set noreorder

    .globl _kernel_entry
    .section .text, "ax"

_kernel_entry:
    lui   $sp, %hi(kernel_stack + 0x1000)
    addiu $sp, $sp, %lo(kernel_stack + 0x1000)
    j     kernelMain
    nop
