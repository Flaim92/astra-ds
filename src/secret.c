/* Токен: живёт только в закреплённых страницах (не уходят в подкачку и в дампы),
 * хранится только в системном хранилище секретов, вводится скрыто. Копии затираются. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc.h"
#include "plat.h"

#define TOKEN_MAX 256
#define SCRATCH_CAP 8192

#define VLESS_AREA 16384

/* Раскладка защищённой области: [токен][заголовок Authorization][scratch для IDENTIFY/RESUME]
 * [область VLESS: ссылка, разобранные поля, логин и пароль локального SOCKS — её размечает vless.c] */
typedef struct {
    char token[TOKEN_MAX];
    char auth[TOKEN_MAX + 32];
    char scratch[SCRATCH_CAP];
    union { char raw[VLESS_AREA]; max_align_t align; } vless;
} Secret;

/* Временно здесь, до переноса в mc.h (раздел secret.c): защищённая область для vless.c,
 * NULL — область не выделена (secret_init не вызван или уже secret_destroy). */
void *secret_vless_area(size_t *cap);

static Secret *g_sec;
static size_t g_sec_size;

bool secret_init(void) {
    g_sec_size = (sizeof(Secret) + 4095) & ~(size_t)4095;
    g_sec = plat_pages(g_sec_size);
    if (!g_sec) return false;
    plat_lock(g_sec, g_sec_size);
    return true;
}

static bool token_ok(const char *t, size_t n) {
    if (n < 20 || n >= TOKEN_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

void secret_set(const char *token, size_t n) {
    plat_wipe(g_sec->token, sizeof g_sec->token);
    plat_wipe(g_sec->auth, sizeof g_sec->auth);
    if (!token_ok(token, n)) return;
    memcpy(g_sec->token, token, n);
    memcpy(g_sec->auth, "Authorization: ", 15);
    memcpy(g_sec->auth + 15, token, n);
}
const char *secret_token(void) { return g_sec && g_sec->token[0] ? g_sec->token : NULL; }
const char *secret_auth_header(void) { return g_sec->auth; }
char *secret_scratch(size_t *cap) { *cap = SCRATCH_CAP; return g_sec->scratch; }
void *secret_vless_area(size_t *cap) {
    *cap = g_sec ? VLESS_AREA : 0;
    return g_sec ? g_sec->vless.raw : NULL;
}

void secret_destroy(void) {
    if (!g_sec) return;
    plat_wipe(g_sec, g_sec_size);
    plat_unlock(g_sec, g_sec_size);
    plat_pages_free(g_sec, g_sec_size);
    g_sec = NULL;
}

/* Временные буферы для обмена с хранилищем — тоже в защищённой области (scratch). */
bool cred_load(void) {
    size_t cap;
    char *buf = secret_scratch(&cap);
    int n = keyring_load("token", buf, TOKEN_MAX);
    bool ok = false;
    if (n > 0) { secret_set(buf, (size_t)n); ok = secret_token() != NULL; }
    plat_wipe(buf, cap);
    return ok;
}
bool cred_save(void) {
    const char *t = secret_token();
    return t && keyring_save("token", t, strlen(t));
}
void cred_delete(void) { keyring_delete("token"); }

/* ---------- несколько аккаунтов ----------
 * "accounts" — строки "id\tимя\n" (до ACC_MAX); "acct:<id>" — токен аккаунта. Имя — только для списка
 * (без управляющих символов и табуляции, до 63 байт по границе UTF-8). */
static bool acct_on(void) { return !O.no_save && !O.demo; }
static void acct_key(char *k, size_t cap, u64 uid) { snprintf(k, cap, "acct:%llu", (unsigned long long)uid); }
static int acct_read(AcctInfo *a, int max) {
    char b[ACC_MAX * 96 + 1];
    int n = keyring_load("accounts", b, sizeof b - 1);
    if (n <= 0) return 0;
    b[n] = 0;
    int k = 0;
    for (char *p = b; *p && k < max; ) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        char *t = strchr(p, '\t');
        unsigned long long id = strtoull(p, NULL, 10);
        if (t && id) {
            a[k].id = id;
            snprintf(a[k].name, sizeof a[k].name, "%s", t + 1);
            k++;
        }
        if (!e) break;
        p = e + 1;
    }
    return k;
}
static bool acct_write(const AcctInfo *a, int n) {
    if (n <= 0) { keyring_delete("accounts"); return true; }
    char b[ACC_MAX * 96];
    size_t len = 0;
    for (int i = 0; i < n && i < ACC_MAX; i++) {
        int w = snprintf(b + len, sizeof b - len, "%llu\t%s\n", (unsigned long long)a[i].id, a[i].name);
        if (w < 0 || (size_t)w >= sizeof b - len) break;
        len += (size_t)w;
    }
    return keyring_save("accounts", b, len);
}
int acct_list(AcctInfo *out, int max) { return acct_on() ? acct_read(out, max) : 0; }
bool acct_save_current(u64 uid, const char *name) {
    const char *t = secret_token();
    if (!acct_on() || !uid || !t) return false;
    AcctInfo a[ACC_MAX];
    int n = acct_read(a, ACC_MAX), at = -1;
    for (int i = 0; i < n; i++) if (a[i].id == uid) at = i;
    if (at < 0 && n >= ACC_MAX) return false;    /* список полон: новый не добавляем */
    char k[40];
    acct_key(k, sizeof k, uid);
    if (at >= 0 && !strncmp(a[at].name, name ? name : "", sizeof a[at].name - 1)) {   /* уже есть: писать, только если токен другой */
        size_t cap;
        char *buf = secret_scratch(&cap);
        int m = keyring_load(k, buf, TOKEN_MAX);
        bool same = m > 0 && (size_t)m == strlen(t) && !memcmp(buf, t, (size_t)m);
        plat_wipe(buf, cap);
        if (same) return true;
    }
    if (!keyring_save(k, t, strlen(t))) return false;
    if (at < 0) { at = n++; a[at].id = uid; }
    /* имя: без управляющих и табуляции, обрезка по границе символа UTF-8 */
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p && o < sizeof a[at].name - 1; p++) {
        if (*p < 0x20 || *p == 0x7F) continue;
        a[at].name[o++] = (char)*p;
    }
    if (o == sizeof a[at].name - 1)              /* срезано: не оставлять начало многобайтового символа */
        while (o > 0 && ((unsigned char)a[at].name[o - 1] & 0x80)) { unsigned char c = (unsigned char)a[at].name[--o]; if ((c & 0xC0) == 0xC0) break; }
    a[at].name[o] = 0;
    return acct_write(a, n);
}
bool acct_load(u64 uid) {
    if (!acct_on() || !uid) return false;
    size_t cap;
    char *buf = secret_scratch(&cap);
    char k[40];
    acct_key(k, sizeof k, uid);
    int n = keyring_load(k, buf, TOKEN_MAX);
    bool ok = n > 0 && token_ok(buf, (size_t)n);
    if (ok) secret_set(buf, (size_t)n);
    plat_wipe(buf, cap);
    return ok && secret_token() != NULL;
}
void acct_delete(u64 uid) {
    if (!acct_on() || !uid) return;
    char k[40];
    acct_key(k, sizeof k, uid);
    keyring_delete(k);
    AcctInfo a[ACC_MAX];
    int n = acct_read(a, ACC_MAX), m = 0;
    for (int i = 0; i < n; i++) if (a[i].id != uid) a[m++] = a[i];
    if (m != n) acct_write(a, m);
}
void acct_wipe_all(void) {
    AcctInfo a[ACC_MAX];
    int n = acct_read(a, ACC_MAX);
    for (int i = 0; i < n; i++) { char k[40]; acct_key(k, sizeof k, a[i].id); keyring_delete(k); }
    keyring_delete("accounts");
}

bool read_hidden(const char *prompt) {
    size_t cap;
    char *buf = secret_scratch(&cap);
    bool ok = term_read_secret(prompt, buf, TOKEN_MAX);
    if (ok) secret_set(buf, strlen(buf));
    plat_wipe(buf, cap);
    return ok && secret_token() != NULL;
}
