/* SysV/AAPCS aggregate FP returns are not yet classified by KCC. */
typedef struct { float x, y, z; } V3;
static V3 make(void) { V3 v = { 1.0f, 2.0f, 3.0f }; return v; }
int main(void) { V3 v = make(); return (int)(v.x + v.y + v.z); }
