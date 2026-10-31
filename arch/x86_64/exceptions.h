#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdint.h>

void exception_handler_main(uint64_t vector, uint64_t err_code, uint64_t rip, uint64_t cr2, uint64_t cs, const uint64_t* frame);

#endif
