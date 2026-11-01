struct P { int x, y; };
int sum(struct P *p) { return p->x + p->y; }
int main(void) {
    int *a = (int[]){3, 4, 5};
    struct P p = (struct P){10, 20};
    return sum(&(struct P){1, 2}) + a[2] + p.y + ((struct P){7, 8}).y;
}
