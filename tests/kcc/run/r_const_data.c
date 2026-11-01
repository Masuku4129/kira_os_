const int tbl[8] = {1, 1, 2, 3, 5, 8, 13, 21};
static const char hex[] = "0123456789abcdef";
int main(void) {
    int s = 0;
    for (int i = 0; i < 8; i++) s += tbl[i] * (i + 1);
    char out[9]; unsigned v = 0xCAFE;
    for (int i = 0; i < 4; i++) out[i] = hex[(v >> (12 - 4 * i)) & 15];
    out[4] = 0;
    return s + out[0] + out[3];
}
