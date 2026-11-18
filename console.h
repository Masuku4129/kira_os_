#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void console_init(void);
/* Связь с оболочкой рабочего стола (уровень 8): консоль (уровень 6) её не знает, оболочка подключает себя сама (wire.c). Нет оболочки - нет и действий. */
typedef struct {
    void (*print_info)(void);                                  /* команда fb */
    void (*request_files)(const char* left, const char* right); /* команда files */
    int  (*kbd_owner)(void);                                   /* 1 - клавиатуру читает Проводник */
    void (*files_state)(void);                                 /* команда fmstate: строка состояния Проводника */
} console_shell_t;
void console_set_shell(const console_shell_t* sh);
void console_graceful_poweroff(void);   // сохранить опыт и диски, выключить (кнопка питания ACPI)
void console_power_prepare(void);       // сохранить опыт Разума, отложенную запись ФС и кэши дисков (как перед выключением; сон S3 делает то же)
void console_poll(void);               // обработать накопившийся ввод с UART (COM1)
// Команда от оболочки (Проводник и т.п.), из любого потока: выполнится в основном потоке ядра при следующем
// console_poll так, будто её набрали в терминале (с эхом после приглашения). 0 - принята, -1 - очередь полна.
int  console_request(const char* line);
int  console_request_src(const char* line, unsigned src);   /* то же с источником для журнала аудита (AUD_SRC_RCON для удалённой консоли) */
/* Стандартный ввод программы (системный вызов STDIN, право CAP_STDIN). Пока программа владеет вводом, набранные строки идут ей, а не оболочке.
   begin: 0 - программа владеет вводом, < 0 - им владеет другая. poll: байт строки (с \n); 0 - конец ввода (Ctrl+D); -11 - строки пока нет;
   -4 - прервано (Ctrl+C возвращает ввод оболочке) или владелец другой. release: программа завершилась. */
int     console_stdin_begin(uint32_t pid);
int64_t console_stdin_poll(uint32_t pid, char* out, uint64_t cap);
void    console_stdin_release(uint32_t pid);

#endif
