#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdint.h>

// Поток дисплея: единственный, кто выводит кадры на экран. Забирает журнал ядра в
// текстовую консоль, показывает окна (оболочка и приложения Ring 3, в том числе 3D)
// только изменившимися областями и держит темп кадров по наносекундным часам.
// Основа для будущего композитора (окна, курсор, GUI): все клиенты рисуют в поверхности,
// показывает их один поток.
int  display_init(unsigned long mbi_addr);   // 0 - экран есть; <0 - работаем без него
void display_print_info(void);
/* Команда «files»: открыть Проводник; левая и правая панель - на этих каталогах ("" - не менять). Из любого потока. */
void display_request_files(const char* left, const char* right);
int  display_kbd_owner(void);                /* 1 - клавиатуру забрал Launcher или Проводник; консоль не читает */
void display_panic_flush(void);              // синхронно вывести журнал на экран (паника/исключение)
// Для Разума: циклов дисплея и опозданий с начала (растут), худший промежуток между циклами с прошлого вызова.
void display_get_stats(uint64_t* loops, uint64_t* late, uint64_t* worst_gap_ns);
void display_get_render_stats(uint64_t* frames, uint64_t* composed, uint64_t* pixels, uint64_t* busy_ns, uint64_t* worst_busy_ns, uint64_t* last_present_ns);
void display_set_fx(int level);
/* Потокобезопасная очередь уведомлений для служб/драйверов; 0 — принято, -1 — неверный ввод/дисплей не готов, -2 — очередь заполнена. */
int display_notify(const char* title, const char* message, int priority);
/* Композитор сам выбирает, собирать кадр на всех ядрах или на одном: по замеренной цене на этой машине (на эмуляторе
   с одним настоящим потоком параллельная сборка только дороже). par - сейчас параллельно; цена 1000 точек, нс. */
void display_compositor_stats(uint32_t* par, uint32_t* ns_kpx_par, uint32_t* ns_kpx_ser);               // уровень анимации фона 0..3 (решает Разум)

#endif
