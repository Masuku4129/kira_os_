#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
static int feed(const uint8_t* s, int n, input_event_t* out) {
    int cnt = 0; for (int i = 0; i < n; i++) { input_push_scancode(s[i]); }
    input_event_t e; while (input_pop(&e)) { out[cnt++] = e; } return cnt;
}
int main(void) {
    input_event_t ev[16]; input_init();
    uint8_t a[] = {0x1E, 0x9E};                       /* a вниз/вверх */
    CHECK(feed(a,2,ev)==2); CHECK(ev[0].ch=='a' && ev[0].type==INPUT_KEY_DOWN); CHECK(ev[1].type==INPUT_KEY_UP && ev[1].ch==0);
    uint8_t sh[] = {0x2A, 0x1E, 0x9E, 0xAA};          /* Shift+a -> 'A' */
    CHECK(feed(sh,4,ev)==4); CHECK(ev[1].ch=='A' && (ev[1].mods&INPUT_MOD_SHIFT)); CHECK(!(ev[3].mods&INPUT_MOD_SHIFT));
    uint8_t num[] = {0x2A, 0x02, 0x82, 0xAA};         /* Shift+1 -> '!' */
    CHECK(feed(num,4,ev)==4); CHECK(ev[1].ch=='!');
    uint8_t caps[] = {0x3A, 0xBA, 0x1E, 0x9E, 0x3A, 0xBA, 0x1E, 0x9E};
    CHECK(feed(caps,8,ev)==8); CHECK(ev[0].key==KEY_CAPS);
    CHECK(ev[2].ch=='A'); CHECK(ev[6].ch=='a');
    uint8_t arr[] = {0xE0, 0x48, 0xE0, 0xC8};         /* стрелка вверх */
    input_init(); CHECK(feed(arr,4,ev)==2); CHECK(ev[0].key==KEY_UP && ev[1].type==INPUT_KEY_UP);
    uint8_t ctl[] = {0x1D, 0x2E, 0xAE, 0x9D};         /* Ctrl+C -> 0x03 */
    CHECK(feed(ctl,4,ev)==4); CHECK(ev[1].ch==3);
    /* Ctrl+Alt+Left/Right are shell workspace shortcuts, not app key events. */
    input_init();
    uint8_t ws_prev[] = {0x1D, 0x38, 0xE0, 0x4B, 0xE0, 0xCB, 0xB8, 0x9D};
    CHECK(feed(ws_prev, (int)sizeof ws_prev, ev) == 4); /* only modifier events remain in the app queue */
    for (int i = 0; i < 4; i++) CHECK(ev[i].key != KEY_LEFT && ev[i].key != KEY_RIGHT);
    CHECK(input_take_hotkey() == KEY_WORKSPACE_PREV && input_take_hotkey() == 0);
    uint8_t ws_next[] = {0x1D, 0x38, 0xE0, 0x4D};
    (void)feed(ws_next, (int)sizeof ws_next, ev);
    CHECK(input_take_hotkey() == KEY_WORKSPACE_NEXT);
        uint8_t junk[] = {0xFA, 0xE1, 0x00, 0xFF};        /* ACK/Pause/ошибки: без событий */
    input_init(); CHECK(feed(junk,4,ev)==0);
    uint8_t ent[] = {0x1C, 0x9C};
    CHECK(feed(ent,2,ev)==2); CHECK(ev[0].ch=='\n');
    input_init();                                      /* переполнение кольца */
    for (int i = 0; i < 300; i++) input_push_scancode(0x1E);
    CHECK(input_dropped() == 300 - 128);
    /* мышь: пакет, знаки, кнопки, ресинхронизация, переполнение */
    input_init(); mouse_event_t m;
    input_push_mouse_byte(0x09); input_push_mouse_byte(5); input_push_mouse_byte(3);   /* L, dx=+5, dy вверх 3 */
    CHECK(input_pop_mouse(&m) && m.dx == 5 && m.dy == -3 && m.buttons == 1);
    input_push_mouse_byte(0x38); input_push_mouse_byte(0xFB); input_push_mouse_byte(0xFE); /* знаки: dx=-5, dy=-2 (вниз 2) */
    CHECK(input_pop_mouse(&m) && m.dx == -5 && m.dy == 2 && m.buttons == 0);
    input_push_mouse_byte(0x00);                                                       /* мусор: бит 3 не стоит -> пропуск */
    input_push_mouse_byte(0x0C); input_push_mouse_byte(1); input_push_mouse_byte(1);
    CHECK(input_pop_mouse(&m) && m.buttons == 4 && m.dx == 1);
    input_push_mouse_byte(0x49); input_push_mouse_byte(1); input_push_mouse_byte(1);   /* бит переполнения X */
    CHECK(!input_pop_mouse(&m));
    for (int i = 0; i < 100; i++) { input_push_mouse_byte(0x08); input_push_mouse_byte(0); input_push_mouse_byte(0); }
    int cnt = 0; while (input_pop_mouse(&m)) cnt++;
    CHECK(cnt == 64);
    /* кириллица: ЙЦУКЕН, Shift и Caps - заглавные, Ctrl/Alt - без перевода, не буквы - как есть */
    if (input_cyr('q', 'q', 0) != 0x439 || input_cyr('q', 'Q', INPUT_MOD_SHIFT) != 0x419) { printf("FAIL cyr q\n"); return 1; }
    if (input_cyr('`', '`', 0) != 0x451 || input_cyr('`', '~', INPUT_MOD_SHIFT) != 0x401) { printf("FAIL cyr yo\n"); return 1; }
    if (input_cyr(';', ';', 0) != 0x436 || input_cyr('f', 'F', INPUT_MOD_CAPS) != 0x410) { printf("FAIL cyr zh/a\n"); return 1; }
    if (input_cyr('f', 'f', INPUT_MOD_SHIFT | INPUT_MOD_CAPS) != 0x430) { printf("FAIL cyr shift+caps\n"); return 1; }
    if (input_cyr('/', '/', 0) != '.' || input_cyr('/', '?', INPUT_MOD_SHIFT) != ',') { printf("FAIL cyr dot\n"); return 1; }
    if (input_cyr('c', 'c', INPUT_MOD_CTRL) != 'c' || input_cyr('1', '1', 0) != '1' || input_cyr(KEY_ENTER, 0, 0) != 0) { printf("FAIL cyr passthrough\n"); return 1; }
    printf("input: all decoder checks passed (incl. Cyrillic layout)\n");
    return 0;
}
