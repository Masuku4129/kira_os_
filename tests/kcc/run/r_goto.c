int main(void) {
    int i = 0, s = 0;
again:
    if (i >= 6) goto out;
    s += i * i; i++;
    goto again;
out:
    for (i = 0; i < 3; i++) { if (i == 1) goto skip; s += 100; skip: s += 1; }
    return s;
}
