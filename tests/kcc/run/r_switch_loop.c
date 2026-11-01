int main(void) {
    int s = 0;
    for (int i = 0; i < 12; i++) {
        switch (i % 5) {
        case 0: continue;
        case 1: s += 1; break;
        case 3: s += 30;
        case 4: s += 4; if (i > 8) goto end; break;
        default: s += 2;
        }
        s += 100;
    }
end:
    return s;
}
