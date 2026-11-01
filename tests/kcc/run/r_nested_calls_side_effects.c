int g;
int bump(int d) { g += d; return g; }
int pick(int a, int b) { return a * 10 + b; }
int main(void) {
    int r = pick(bump(1), 2) + pick(3, bump(0));
    int arr[3] = {0, 0, 0}; int i = 0;
    arr[i] = i + 5; i = 1; arr[i] += 7; arr[2] = arr[1]++ ;
    return r + arr[0] + arr[1] * 10 + arr[2] * 100 + g;
}
