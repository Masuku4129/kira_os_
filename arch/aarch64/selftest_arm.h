#ifndef SELFTEST_ARM_H
#define SELFTEST_ARM_H
/* Проверка общего ядра на этой машине (команда test): потоки на всех ядрах, спин-замок, вытеснение по кванту,
   мьютекс ядра со сном, точность сна. Итог печатается последним завершившимся потоком: RESULT: PASS | FAIL. */
void selftest_arm_start(void);
#endif
