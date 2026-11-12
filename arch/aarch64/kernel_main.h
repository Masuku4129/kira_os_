#ifndef KIRA_ARM64_KERNEL_MAIN_H
#define KIRA_ARM64_KERNEL_MAIN_H

/* ARM64 steady-state core loop; platform discovery and hardware bring-up stay in main.c. */
void arm_kernel_main_loop(void);

#endif
