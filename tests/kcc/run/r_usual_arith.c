/* LP64 usual arithmetic conversions: signed long can represent unsigned int,
   but signed long long cannot represent unsigned long of equal width. */
int main(void) {
    int a = ((long)-1 < (unsigned int)1);
    int b = ((unsigned long)1 < (long long)-1);
    int c = ((long long)-1 < (unsigned long)1);
    int d = ((unsigned int)0xffffffffu < (long)0);
    int e = ((unsigned long long)1 > (long)-1);
    int f = ((unsigned long long)1 < (long)-1);
    int g = ((int)-1 < (unsigned int)1);
    return a + 2*b + 4*c + 8*d + 16*e + 32*f + 64*g;
}
