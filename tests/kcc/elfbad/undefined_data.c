// expect: undefined reference to 'missing_var'
extern int missing_var;
int main(void) { return missing_var; }
