#include <vector>
#include <cstdio>
int main() {
    std::vector<int> v(4, 1); int &r = v[0];       /* ссылка на элемент */
    for (int i = 0; i < 1000; i++) v.push_back(i);  /* перевыделение: r повисла */
    int *p = new int(1234); delete p; int *q = new int(5678);
    std::printf("r=%d p[0]=%d q[0]=%d\n", r, *p, *q);   /* два использования после освобождения */
    delete q;
}
