int main(void) {
    char c = 127; unsigned char u = 255; short s = 32767; unsigned short us = 0; long l = -1;
    int arr[4] = {10, 20, 30, 40}; int *p = arr; int r = 0;
    r += c++; r += ++c;
    r += u++; r += u; r += --us; r += s++; r += s; r += (int)l--; r += (int)l;
    r += *p++; r += *++p; r += p[-1]; p--; r += (*p)++; r += *p; r += arr[1]--;
    r += --arr[2]; r += arr[3]++; r += arr[3];
    return r;
}
