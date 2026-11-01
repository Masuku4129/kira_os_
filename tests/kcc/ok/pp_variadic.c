#define LOG(fmt, ...) printf(fmt, __VA_ARGS__)
int printf(const char *, ...);
void f(void) { LOG("%d %d", 1, 2); LOG("%s", "x"); }
