#ifndef THEME_H
#define THEME_H

/*
 * KIRA — единая тема графической оболочки (минималистичная, для рабочих мест и промышленных комплексов).
 * Описание и обоснование: docs/UI_DESIGN.md. Цвета заданы как 0x00RRGGBB.
 *
 * Правила палитры (ISA-101): основа нейтральная серо-синяя; один акцент (выбор, фокус, ссылки);
 * цвет несёт смысл ТОЛЬКО в трёх состояниях: норма (зелёный), внимание (янтарный), авария (красный).
 * Контраст текста к поверхностям не ниже 4,5:1 (WCAG AA / ГОСТ Р ИСО 9241-3); проверяется
 * хост-тестом tests/theme_contrast_test.c.
 */

/* ---- поверхности (от дальней к ближней) ---- */
#define TH_SURF_DESK    0x001C2229u   /* рабочий стол */
#define TH_SURF_BAR     0x00232B34u   /* верхняя панель, док, строки состояния */
#define TH_SURF_WIN     0x00232B34u   /* корпус окна */
#define TH_SURF_HEAD    0x002B3540u   /* заголовки, панель инструментов, шапка таблицы */
#define TH_SURF_CONTENT 0x001F262Eu   /* содержимое окон */
#define TH_SURF_ROW_ALT 0x00222A32u   /* чётная строка списка (лёгкая зебра) */
#define TH_SURF_TRACK   0x002A343Fu   /* дорожки, поля ввода */
#define TH_EDGE         0x00414E5Cu   /* линии и рамки */

/* ---- текст ---- */
#define TH_FG           0x00E8EDF2u   /* основной */
#define TH_FG_DIM       0x00B3BDC8u   /* вторичный */
#define TH_FG_FAINT     0x0095A1AEu   /* подписи, недоступное (всё равно >= 4,5:1) */

/* ---- акцент (единственный): выбор, фокус, активный элемент ---- */
#define TH_ACCENT       0x006FAEEAu

/* ---- состояния (только по смыслу, не для украшения) ---- */
#define TH_OK           0x005CC08Au   /* норма / успех */
#define TH_WARN         0x00F0B84Au   /* внимание */
#define TH_ALARM        0x00FF7B72u   /* авария / опасное действие */

/* ---- шкала размеров (сетка 4/8/16) ---- */
#define TH_S1           4
#define TH_S2           8
#define TH_S3           16
#define TH_BAR_H        24            /* высота полос и кнопок: цель клика не меньше 24 px */
#define TH_ROW_H        20            /* строка списка */
#define TH_GAP          TH_S2
#define TH_RADIUS       4             /* скругление: едва заметное */

/* ---- прежние имена (псевдонимы, чтобы код приложений не менялся разом) ---- */
#define TH_BG0          TH_SURF_DESK
#define TH_BG1          TH_SURF_DESK
#define TH_BAR0         TH_SURF_BAR
#define TH_BAR1         TH_SURF_BAR
#define TH_PANEL0       TH_SURF_HEAD
#define TH_PANEL1       TH_SURF_WIN
#define TH_CONTENT      TH_SURF_CONTENT
#define TH_TILE         TH_SURF_HEAD
#define TH_BORDER       TH_EDGE
#define TH_GRID         TH_EDGE
#define TH_CYAN         TH_ACCENT     /* бывшие «цветные» акценты слиты в один */
#define TH_BLUE         TH_ACCENT
#define TH_PURPLE       TH_ACCENT
#define TH_PINK         TH_FG_DIM
#define TH_GREEN        TH_OK
#define TH_AMBER        TH_WARN
#define TH_RED          TH_ALARM
#define TH_TEXT         TH_FG
#define TH_TEXT_DIM     TH_FG_DIM
#define TH_TEXT_FAINT   TH_FG_FAINT
#define TH_CARD         TH_SURF_WIN
#define TH_CARD_EDGE    TH_EDGE
#define TH_TRACK        TH_SURF_TRACK
#define TH_ROW_ALT      TH_SURF_ROW_ALT

/* Прежние цвета кнопок окна (оставлены как псевдонимы; сами кнопки теперь нейтральные, красным при наведении
   становится только «закрыть» - действие необратимо) */
#define TH_CLOSE        TH_ALARM
#define TH_MIN          TH_WARN
#define TH_FULL         TH_OK

#endif
