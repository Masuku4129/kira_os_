// expect: undefined reference to 'missing_fn'
int missing_fn(int);
int main(void) { return missing_fn(3); }
