#define LOG(fmt, args...) printf(fmt, ## args)
int printf(const char *, ...);
void f(void) { LOG("a"); LOG("%d", 1); }
