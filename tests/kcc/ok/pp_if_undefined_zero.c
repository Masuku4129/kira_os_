#if UNDEFINED_THING
int bad(;
#endif
#if UNDEFINED_THING == 0 && !UNDEFINED_THING
int ok;
#endif
