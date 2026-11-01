unsigned slen(const char *s) { unsigned n = 0; while (*s++) n++; return n; }
int scmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *(unsigned char *)a - *(unsigned char *)b; }
void scpy(char *d, const char *s) { while ((*d++ = *s++)) ; }
int main(void) {
    char buf[16] = "abc"; char big[8] = {'x', 'y'}; char t[] = "hello";
    scpy(buf + 3, "defg");
    return slen(buf) * 100 + scmp("abc", "abd") + scmp(buf, "abcdefg") + slen(t) + big[1] + big[5] + sizeof(t);
}
