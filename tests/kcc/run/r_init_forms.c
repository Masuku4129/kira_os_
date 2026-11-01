struct P { int x, y, z; };
struct Q { struct P p; int a[3]; char s[4]; };
int g1[6] = {1, 2, [4] = 5, 6};
struct Q gq = {{1, 2, 3}, {4, 5}, "hi"};
struct P gp[3] = {{1}, {.z = 9}, 7, 8, 9};
int main(void) {
    int l1[5] = {1, 2}; int l2[] = {5, 4, 3, 2, 1};
    struct Q lq = {.a = {[2] = 7}, .p.y = 5, .s = "x"};
    struct P lp[2] = {1, 2, 3, 4};
    int m[2][3] = {{1}, {2, 3}};
    return g1[0] + g1[3] + g1[4] + g1[5] + gq.p.z + gq.a[1] + gq.a[2] + gq.s[1] + gp[0].x + gp[1].z + gp[2].y +
        l1[1] + l1[4] + (int)(sizeof l2 / sizeof l2[0]) + l2[3] + lq.a[2] + lq.p.y + lq.s[0] + lq.p.x + lp[1].x + m[1][1] + m[0][2];
}
