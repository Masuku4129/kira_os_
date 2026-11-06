#ifndef THEME_H
#define THEME_H

/*
 * KIRA Aurora — единая тема графической оболочки.
 * Нейтральная глубокая основа, спокойные акценты и повышенная читаемость.
 * Цвета заданы как 0x00RRGGBB; прозрачность задаётся отдельными параметрами UI.
 */

/* Рабочий стол и системные поверхности */
#define TH_BG0        0x00080F1Cu   /* верхний фон */
#define TH_BG1        0x000E192Au   /* нижний фон */
#define TH_BAR0       0x000D1727u   /* верхняя панель */
#define TH_BAR1       0x00111F32u
#define TH_PANEL0     0x001A2940u   /* заголовок окна */
#define TH_PANEL1     0x00121F32u   /* рамка/корпус окна */
#define TH_CONTENT    0x000B1626u   /* содержимое окон */
#define TH_TILE       0x0016263Bu   /* плитки запуска */
#define TH_BORDER     0x002B3D57u
#define TH_GRID       0x00213B58u

/* Акценты: различимы на тёмных поверхностях, без чрезмерного свечения */
#define TH_CYAN       0x005EDCF5u
#define TH_BLUE       0x006B9EFFu
#define TH_PURPLE     0x00B18AFFu
#define TH_GREEN      0x0054D6A5u
#define TH_AMBER      0x00F4C46Bu
#define TH_RED        0x00FF7C86u
#define TH_PINK       0x00F08BB9u

/* Текст: высокий контраст основного текста, мягкие вторичные подписи */
#define TH_TEXT       0x00F0F4FAu
#define TH_TEXT_DIM   0x00AAB9CCu
#define TH_TEXT_FAINT 0x007889A2u

/* Общие компоненты приложений: карточки, кнопки, строки и полосы состояния */
#define TH_CARD       0x00132135u
#define TH_CARD_EDGE  0x002A3D57u
#define TH_TRACK      0x001C2D44u
#define TH_ROW_ALT    0x000F1B2Cu
#define TH_BAR_H      24
#define TH_GAP        10
#define TH_RADIUS     10

/* Кнопки управления окнами */
#define TH_CLOSE      0x00FF6B68u
#define TH_MIN        0x00F2BE58u
#define TH_FULL       0x004CCB83u

#endif
