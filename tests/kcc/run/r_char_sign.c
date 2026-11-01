int main(void) {
    char c = 200; unsigned char u = 200; signed char sc = 200;
    int r = (c < 0) + (u > 100) * 2 + (sc < 0) * 4 + c / 3 + u / 3 + (c >> 1) + (u >> 1);
    char buf[3]; buf[0] = 'a'; buf[1] = (char)0xFF; buf[2] = 0;
    r += buf[1] + (unsigned char)buf[1] + (buf[1] == -1) + (buf[1] == 255);
    for (unsigned char k = 250; k != 4; k++) r++;
    return r;
}
