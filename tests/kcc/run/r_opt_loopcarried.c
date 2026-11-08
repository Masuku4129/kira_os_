/* Оптимизатор: переменная, присваиваемая условно внутри цикла, сохраняет значение между итерациями. */
int main(void) {
    int x = 7, y = 0;
    for (int i = 0; i < 10; i++) {
        if (i == 3) x = 100;
        if (i % 2) y += x;
        else y -= 1;
    }
    int z;
    int first = 1, last = 0;
    for (int i = 0; i < 5; i++) {
        if (first) { z = 40; first = 0; }
        last += z + i;
    }
    return (y + last) & 0xffffff;
}
