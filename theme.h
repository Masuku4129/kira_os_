#ifndef THEME_H
#define THEME_H

/* Палитра оболочки «ОС Кира»: тёмно-синяя ночь, неоновые голубой, фиолетовый и зелёный. */
#define TH_BG0        0x00050A16u   /* фон рабочего стола: верх */
#define TH_BG1        0x000B1630u   /* фон рабочего стола: низ */
#define TH_BAR0       0x00081022u   /* верхняя панель */
#define TH_BAR1       0x00060C1Au
#define TH_PANEL0     0x000C1629u   /* окно: верх */
#define TH_PANEL1     0x00091120u   /* окно: низ */
#define TH_CONTENT    0x00070D19u   /* поле содержимого окон ядра */
#define TH_TILE       0x000C1830u   /* плитка боковой панели */
#define TH_BORDER     0x001C2C48u
#define TH_GRID       0x0022D3EEu

#define TH_CYAN       0x0022D3EEu   /* неоновый голубой */
#define TH_BLUE       0x003B82F6u
#define TH_PURPLE     0x00A855F7u   /* неоновый фиолетовый */
#define TH_GREEN      0x0034D399u   /* неоновый зелёный */
#define TH_AMBER      0x00FBBF24u
#define TH_RED        0x00F87171u
#define TH_PINK       0x00F472B6u

#define TH_TEXT       0x00E2E8F0u
#define TH_TEXT_DIM   0x008DA2C0u
#define TH_TEXT_FAINT 0x00566B8Eu

/* Единый вид приложений оболочки (Проводник, Монитор, Разум, Настройки): карточки, кнопки, полоса состояния */
#define TH_CARD       0x000B1528u   /* карточка внутри окна */
#define TH_CARD_EDGE  0x00182844u
#define TH_TRACK      0x00132139u   /* жёлоб, неактивная кнопка */
#define TH_ROW_ALT    0x00091426u   /* чередование строк списка */
#define TH_BAR_H      24            /* высота полосы состояния и полосы подсказки клавиш */
#define TH_GAP        10            /* отступ между карточками и от края окна */
#define TH_RADIUS     10            /* скругление карточек */

/* «Светофор» окна */
#define TH_CLOSE      0x00FF5F57u
#define TH_MIN        0x00FEBC2Eu
#define TH_FULL       0x0028C840u

#endif
