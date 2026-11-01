#define LOG(fmt, ...) printf(fmt, ## __VA_ARGS__)
int printf(const char *, ...);
void f(void) { LOG("a"); LOG("%d", 1); LOG("%d%d", 1, 2); }
