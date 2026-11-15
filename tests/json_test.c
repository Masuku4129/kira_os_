/* Тест json.c: строгий разбор, выборка по пути, глубина, раскрытие escape-последовательностей, фаззинг мутациями. */
#include "../json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); bad++; } } while (0)
#define OK(s) CHECK(json_validate(s, strlen(s)) == JSON_OK)
#define BAD(s) CHECK(json_validate(s, strlen(s)) != JSON_OK)

static int get(const char* doc, const char* path, json_val* v) { return json_get(doc, strlen(doc), path, v); }

int main(void) {
    /* допустимые документы */
    OK("0"); OK("-0"); OK("1.5e+10"); OK("-12.0E-3"); OK("true"); OK("false"); OK("null"); OK("\"\""); OK("[]"); OK("{}");
    OK(" \t\r\n[1, 2 ,3 ] \n"); OK("{\"a\":{\"b\":[true,null,\"x\"]}}"); OK("\"\\u00e9\\ud83d\\ude00\\n\\/\"");
    /* недопустимые: каждый случай нарушает одно правило RFC 8259 */
    BAD(""); BAD(" "); BAD("01"); BAD("1."); BAD(".5"); BAD("+1"); BAD("1e"); BAD("--1"); BAD("tru"); BAD("nul"); BAD("True");
    BAD("[1,]"); BAD("[,1]"); BAD("{\"a\":1,}"); BAD("{a:1}"); BAD("{'a':1}"); BAD("{\"a\" 1}"); BAD("[1 2]"); BAD("[1"); BAD("{\"a\":1");
    BAD("\"abc"); BAD("\"a\nb\""); BAD("\"\\x\""); BAD("\"\\u12\""); BAD("\"\\ud800\""); BAD("\"\\udc00\""); BAD("\"\\ud800\\u0041\"");
    BAD("1 2"); BAD("[] []"); BAD("{}}"); BAD("NaN"); BAD("Infinity"); BAD("[1]x");
    { char d[8] = "\"a\0b\""; CHECK(json_validate(d, 5) == JSON_ERR_SYNTAX || json_validate(d, 5) == JSON_OK); }   /* NUL внутри: не падать */
    /* глубина: 16 допустимо, 17 - отказ */
    { char d[64]; int n = 0; for (int i = 0; i < 16; i++) d[n++] = '['; for (int i = 0; i < 16; i++) d[n++] = ']'; d[n] = 0; OK(d);
      n = 0; for (int i = 0; i < 17; i++) d[n++] = '['; for (int i = 0; i < 17; i++) d[n++] = ']'; d[n] = 0; CHECK(json_validate(d, n) == JSON_ERR_DEPTH); }
    { size_t N = 200000; char* d = malloc(N + 1); memset(d, '[', N); d[N] = 0; CHECK(json_validate(d, N) == JSON_ERR_DEPTH); free(d); }   /* без переполнения стека */

    /* выборка по пути */
    json_val v; char s[64];
    const char* doc = "{\"окна\":[{\"ширина\":640,\"имя\":\"a\\tb\"},{\"ширина\":800}],\"on\":true,\"x\":null,\"o\":{\"k\":[1,[2,3]]}}";
    CHECK(get(doc, "", &v) == JSON_OK && v.type == JSON_OBJ && v.n == strlen(doc));
    CHECK(get(doc, "on", &v) == JSON_OK && v.type == JSON_BOOL && v.n == 4);
    CHECK(get(doc, "x", &v) == JSON_OK && v.type == JSON_NULL);
    CHECK(get(doc, "окна[0].ширина", &v) == JSON_OK && v.type == JSON_NUM && v.n == 3 && !memcmp(v.p, "640", 3));
    CHECK(get(doc, "окна[1].ширина", &v) == JSON_OK && v.n == 3 && !memcmp(v.p, "800", 3));
    CHECK(get(doc, "окна[0].имя", &v) == JSON_OK && v.type == JSON_STR && json_string(&v, s, sizeof s) == 3 && !strcmp(s, "a\tb"));
    CHECK(get(doc, "o.k[1][0]", &v) == JSON_OK && v.type == JSON_NUM && v.p[0] == '2');
    CHECK(get(doc, "o.k[1]", &v) == JSON_OK && v.type == JSON_ARR && v.n == 5);
    CHECK(get(doc, "окна[2]", &v) == JSON_ERR_NOTFOUND);
    CHECK(get(doc, "нет", &v) == JSON_ERR_NOTFOUND);
    CHECK(get(doc, "on.x", &v) == JSON_ERR_NOTFOUND);                  /* внутрь скаляра войти нельзя */
    CHECK(get(doc, "окна[", &v) == JSON_ERR_PATH);
    CHECK(get(doc, "окна[x]", &v) == JSON_ERR_PATH);
    CHECK(get(doc, "окна[1234567890123]", &v) == JSON_ERR_PATH);
    CHECK(get(doc, "..", &v) == JSON_ERR_PATH);
    CHECK(get("{\"a\":1,\"a\":2}", "a", &v) == JSON_OK && v.p[0] == '1');   /* повтор ключа: берётся первый, определённо */
    CHECK(get("{\"a\":1", "a", &v) == JSON_ERR_SYNTAX);                   /* частично верный документ не читается */
    /* строки: escape, суррогатная пара, буфер */
    json_val q;
    CHECK(get("\"\\u00e9\\ud83d\\ude00\"", "", &q) == JSON_OK);
    CHECK(json_string(&q, s, sizeof s) == 6 && !memcmp(s, "\xc3\xa9\xf0\x9f\x98\x80", 7));
    CHECK(json_string(&q, s, 5) == JSON_ERR_BUF);
    CHECK(json_string(&q, s, 0) < 0);
    CHECK(get("[1]", "", &q) == JSON_OK && json_string(&q, s, sizeof s) < 0);
    { char k[100]; memset(k, 'k', 60); k[60] = 0; char d[200]; snprintf(d, sizeof d, "{\"%s\":1}", k); CHECK(get(d, k, &v) == JSON_ERR_PATH); CHECK(get(d, "kk", &v) == JSON_ERR_NOTFOUND); }

    /* фаззинг: случайные мутации верных документов; любые байты, ничего не падает (ASan/UBSan), результат всегда определён */
    const char* seeds[] = { doc, "[1,2,{\"a\":[true,false,null,\"\\u0041\\ud83d\\ude00\"]}]", "{\"a\":{\"b\":{\"c\":[[],{}]}}}" };
    unsigned long long seed = 88172645463325252ull;
    #define RND() (seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17, seed)
    long accepted = 0;
    for (int it = 0; it < 200000; it++) {
        char b[256]; const char* sd = seeds[RND() % 3]; size_t n = strlen(sd); memcpy(b, sd, n + 1);
        int muts = 1 + (int)(RND() % 4);
        for (int m = 0; m < muts && n; m++) {
            size_t pos = RND() % n;
            switch (RND() % 4) { case 0: b[pos] = (char)RND(); break; case 1: memmove(b + pos, b + pos + 1, n - pos - 1); n--; break;
                                 case 2: if (n < 250) { memmove(b + pos + 1, b + pos, n - pos); b[pos] = "[]{}\",:\\u0123456789-.eE tfn"[RND() % 27]; n++; } break;
                                 default: n = pos; }
        }
        int r = json_validate(b, n); CHECK(r <= 0 && r >= -5);
        if (r == JSON_OK) { accepted++; json_val w; int g = json_get(b, n, "", &w); CHECK(g == JSON_OK && w.p >= b && w.p + w.n <= b + n); }
        json_val w; int g = json_get(b, n, "a[1].b", &w); CHECK(g <= 0 && g >= -5);
    }
    CHECK(accepted > 0);
    printf("json: %s (принято мутантов: %ld из 200000)\n", bad ? "FAILED" : "all passed", accepted);
    return bad ? 1 : 0;
}
