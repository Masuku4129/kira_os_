struct P { int x, y; };
struct R { struct P a, b; char tag[4]; };
struct R g = {{1, 2}, {3, 4}, "ab"};
int sum(struct R *r) { return r->a.x + r->a.y + r->b.x + r->b.y + r->tag[0] + r->tag[1] + r->tag[2]; }
int main(void) {
    struct R l = g, m;
    l.b.y = 40; m = l; m.a = m.b; m.tag[2] = 'z';
    struct P arr[3] = {{1, 1}, {2, 2}};
    arr[2] = arr[1]; arr[2].x++;
    return sum(&g) + sum(&l) + sum(&m) + arr[2].x * 1000 + arr[0].y;
}
