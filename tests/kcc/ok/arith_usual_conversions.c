/* LP64 usual arithmetic conversions: rank and representability matter,
   not just operand width. Clang and KCC both compile this translation unit. */
char signed_long_keeps_unsigned_int[( (-1L < 1U) ) ? 1 : -1];
char signed_long_long_keeps_unsigned_int[( (-1LL < 1U) ) ? 1 : -1];
char unsigned_long_keeps_unsigned_int[( sizeof(1UL + 1U) == 8 ) ? 1 : -1];
char unsigned_long_long_wins[( (-1LL < 1UL) == 0 ) ? 1 : -1];
char unsigned_long_long_rank[( sizeof(1UL + 1LL) == 8 ) ? 1 : -1];
char signed_long_width[( sizeof(1U + 1L) == 8 ) ? 1 : -1];
char unsigned_int_width[( sizeof(1U + 1) == 4 ) ? 1 : -1];

int kmain(void) {
    return !(signed_long_keeps_unsigned_int[0] == 0 &&
             unsigned_long_long_wins[0] == 0 &&
             unsigned_long_long_rank[0] == 0 &&
             signed_long_width[0] == 0 &&
             unsigned_int_width[0] == 0);
}
