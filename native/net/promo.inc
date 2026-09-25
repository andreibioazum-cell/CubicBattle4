/* Promo codes from the solo match cards.
 *
 * Every card carries a fresh random code (net_promo_new_code): 3 Latin letters and
 * 1 digit in a random position, such as KX7M. Nothing is derived from the nick any
 * more, so a code cannot be worked out in advance, and the promo screen never
 * shows it: the card is the only place the player sees it. The code of the last
 * card is the one that can be redeemed (net_promo_check); an older one is
 * replaced by the next card. The reward stays one per account: promo_used.
 *
 * The cloud gets the code in a PATCH of its own into /users/<nick>, the way settings
 * do in settings_storage.inc: promo holds the code of the last card and promo_used
 * says whether the reward was taken. The separate request is deliberate: until the
 * Firebase rules are republished those PATCHes simply fail and progress, cups,
 * classes and levels, keeps saving.
 *
 * Locally it is promo.dat next to progress.dat:
 *   used 1
 *   code KX7M
 * used says whether the reward was taken on this account and code is the code of
 * the last card, empty when there is none. Unknown lines, such as streak and
 * card_found from older versions, are ignored while reading, and the write is
 * atomic, a temporary file plus rename.
 */
#define PROMO_FILE "promo.dat"
#define PROMO_MAX 24
#define PROMO_CODE_LEN 4

typedef struct {
    DSMutex lock;
    int loaded;
    int used;
    char code[PROMO_MAX];
} Promo;
static Promo pr = { .lock = DS_MUTEX_INIT };
static void pr_lock(void) { ds_mutex_lock(pr.lock); }
static void pr_unlock(void) { ds_mutex_unlock(pr.lock); }
static char s_promo_ret[PROMO_MAX];
/* The card keeps the pointer it got from net_promo_new_code (DimScript strings are
 * plain pointers), so the card code lives in a buffer of its own that only the
 * next card rewrites. */
static char s_promo_card_ret[PROMO_MAX];

/* 3 letters plus 1 digit, the format promo_code_valid (promo.ds) accepts. */
static int promo_code_format_ok(const char *code) {
    int letters = 0, digits = 0;
    if (!code || strlen(code) != PROMO_CODE_LEN) return 0;
    for (int i = 0; i < PROMO_CODE_LEN; i++) {
        char c = code[i];
        if (c >= 'A' && c <= 'Z') letters++;
        else if (c >= '0' && c <= '9') digits++;
        else return 0;
    }
    return letters == 3 && digits == 1;
}

/* Random bytes from the kernel, with the seeded libc generator (srand in
 * main.c) as the fallback when /dev/urandom cannot be read. */
static void promo_random_bytes(unsigned char *out, size_t n) {
    size_t got = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        got = fread(out, 1, n, f);
        fclose(f);
    }
    for (size_t i = got; i < n; i++) out[i] = (unsigned char)(rand() ^ (int)(now_ms() >> (i % 8)));
}

/* A fresh random code. The letters skip I and O and the digit skips 0 and 1, so
 * nothing on the card can be misread when it is typed back in. */
static void promo_random_code(char *out, size_t cap) {
    static const char letters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ";
    static const char digits[] = "23456789";
    unsigned char r[PROMO_CODE_LEN + 1];
    if (!out || cap < PROMO_CODE_LEN + 1) return;
    promo_random_bytes(r, sizeof(r));
    int pos = r[PROMO_CODE_LEN] % PROMO_CODE_LEN;
    for (int i = 0; i < PROMO_CODE_LEN; i++) {
        if (i == pos) out[i] = digits[r[i] % (sizeof(digits) - 1)];
        else out[i] = letters[r[i] % (sizeof(letters) - 1)];
    }
    out[PROMO_CODE_LEN] = 0;
}

static void promo_read(void) {
    char path[320], line[64], code[PROMO_MAX] = "";
    FILE *f;
    int used = 0;
    pr_lock();
    if (pr.loaded) { pr_unlock(); return; }
    pr_unlock();
    data_file_path(path, sizeof(path), PROMO_FILE);
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            int v = 0;
            char s[PROMO_MAX];
            if (sscanf(line, "used %d", &v) == 1) { used = v ? 1 : 0; continue; }
            if (sscanf(line, "code %23s", s) == 1 && promo_code_format_ok(s)) {
                snprintf(code, sizeof(code), "%s", s);
                continue;
            }
        }
        fclose(f);
    }
    pr_lock();
    pr.used = used;
    snprintf(pr.code, sizeof(pr.code), "%s", code);
    pr.loaded = 1;
    pr_unlock();
}

static void promo_write(void) {
    char path[320], text[128];
    int used;
    char code[PROMO_MAX];
    promo_read();
    pr_lock();
    used = pr.used;
    snprintf(code, sizeof(code), "%s", pr.code);
    pr_unlock();
    data_file_path(path, sizeof(path), PROMO_FILE);
    snprintf(text, sizeof(text), "used %d\ncode %s\n", used, code);
    file_write_atomic(path, text);
}

/* The nick of the live session, empty when nobody is logged in. */
static void promo_session_nick(char *nick, size_t cap) {
    nick[0] = 0;
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) snprintf(nick, cap, "%s", lg.session_nick);
    lg_unlock();
}

/* Writes the code of the last card into the cloud profile. */
static void promo_push_code(const char *code) {
    char nick[LOGIN_NICK_MAX + 1];
    promo_session_nick(nick, sizeof(nick));
    if (!nick[0] || !net.base[0] || !promo_code_format_ok(code)) return;
    char url[URL], body[BODY];
    snprintf(url, sizeof(url), "%s/users/%s.json", net.base, nick);
    snprintf(body, sizeof(body), "{\"promo\":\"%s\"}", code);
    http_patch_async(url, body);
    if (net_log_ok()) LOG("promo code of the last card saved for %s", nick);
}

/* The code of the last card, empty when no card was found yet. */
const char *net_promo_code(void) {
    promo_read();
    pr_lock();
    snprintf(s_promo_ret, sizeof(s_promo_ret), "%s", pr.code);
    pr_unlock();
    return s_promo_ret;
}

/* A card was picked up: a fresh random code replaces the previous one, locally
 * and, with a session, in the cloud. The returned string is what the card shows. */
const char *net_promo_new_code(void) {
    char code[PROMO_MAX];
    promo_read();
    pr_lock();
    do {
        promo_random_code(code, sizeof(code));
    } while (strcmp(code, pr.code) == 0); /* the next card never repeats the last code */
    snprintf(pr.code, sizeof(pr.code), "%s", code);
    pr_unlock();
    promo_write();
    promo_push_code(code);
    snprintf(s_promo_card_ret, sizeof(s_promo_card_ret), "%s", code);
    return s_promo_card_ret;
}

/* 1 when the entered code is the code of the last card. */
double net_promo_check(const char *code) {
    double ok;
    if (!promo_code_format_ok(code)) return 0;
    promo_read();
    pr_lock();
    ok = (pr.code[0] && strcmp(pr.code, code) == 0) ? 1 : 0;
    pr_unlock();
    return ok;
}

double net_promo_used(void) {
    double v;
    promo_read();
    pr_lock(); v = (double)pr.used; pr_unlock();
    return v;
}

/* The reward was taken: the flag is set locally and in the cloud, and the code of
 * the card is spent. */
void net_promo_mark_used(void) {
    char nick[LOGIN_NICK_MAX + 1];
    promo_read();
    pr_lock();
    pr.used = 1;
    pr.code[0] = 0;
    pr_unlock();
    promo_write();
    promo_session_nick(nick, sizeof(nick));
    if (!nick[0] || !net.base[0]) return;
    char url[URL], body[BODY];
    snprintf(url, sizeof(url), "%s/users/%s.json", net.base, nick);
    snprintf(body, sizeof(body), "{\"promo_used\":1}");
    http_patch_async(url, body);
    if (net_log_ok()) LOG("promo marked used for %s", nick);
}

/* After a cloud profile load (profile_apply.inc). promo_used=1 in the cloud stops
 * a second reward on a clean device, where the local file may be gone. A card
 * found on another phone brings its code along, so it can be redeemed here; a
 * code that only this device knows (found before the login) goes up. */
int promo_sync_with_cloud(const char *resp) {
    char stored[PROMO_MAX] = "", local[PROMO_MAX];
    char nick[LOGIN_NICK_MAX + 1];
    int changed = 0;
    int used = (int)num(resp, "promo_used", -1);
    strv(resp, "promo", stored, sizeof(stored));
    promo_session_nick(nick, sizeof(nick));
    if (!nick[0]) return 0;
    promo_read();
    pr_lock();
    if (used > 0 && !pr.used) { pr.used = 1; changed = 1; }
    if (pr.used) {
        /* nothing left to redeem on this account */
        if (pr.code[0]) { pr.code[0] = 0; changed = 1; }
    } else if (!pr.code[0] && promo_code_format_ok(stored)) {
        snprintf(pr.code, sizeof(pr.code), "%s", stored);
        changed = 1;
    }
    snprintf(local, sizeof(local), "%s", pr.code);
    pr_unlock();
    if (changed) promo_write();
    if (local[0] && strcmp(local, stored) != 0) {
        promo_push_code(local);
        changed = 1;
    }
    return changed;
}
