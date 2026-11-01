struct N { struct N *next; int v; };
struct N pool[6];
int main(void) {
    struct N *head = 0;
    for (int i = 0; i < 6; i++) { pool[i].v = i * 3 + 1; pool[i].next = head; head = &pool[i]; }
    int s = 0; for (struct N *n = head; n; n = n->next) s = s * 2 + n->v;
    struct N *rev = 0; while (head) { struct N *nx = head->next; head->next = rev; rev = head; head = nx; }
    return s + rev->v * 1000 + rev->next->v;
}
