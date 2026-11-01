struct __attribute__((packed)) P { char a; int b; };
struct Q { char a; int b; } __attribute__((packed));
_Static_assert(sizeof(struct P) == 5 && sizeof(struct Q) == 5, "packed");
