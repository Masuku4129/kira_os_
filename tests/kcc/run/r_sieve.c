char comp[300];
int main(void) {
    int cnt = 0;
    for (int i = 2; i < 300; i++) { if (comp[i]) continue; cnt++; for (int j = i * 2; j < 300; j += i) comp[j] = 1; }
    return cnt;
}
