typedef int I; typedef I *IP; typedef IP IPA[3]; typedef int (*FN)(I);
I f(IPA a, FN fn) { return *a[0] + fn(1); }
