/* The player's personal promo code, the solo match cards and the code entry screen.
 *
 * The code is derived from the nick in a deterministic way, two FNV-1a passes with
 * a project salt, so the same nick always gives the same code and different players
 * get different ones. The code is known offline, and writing a foreign code into
 * the database is useless, because a client recomputes it from its own nick.
 *
 * The cloud gets the code in a PATCH of its own into /users/<nick>, the way settings
 * do in settings_storage.inc: promo holds the code and promo_used says whether it was
 * activated, one reward per account. The separate request is deliberate: until the
 * Firebase rules are republished those PATCHes simply fail and progress, cups, classes
 * and levels, keeps saving.
 *
 * Locally it is promo.dat next to progress.dat:
 *   used 1
 *   streak 7
 * used says whether the code was activated on this device and streak counts the
 * consecutive matches without a card, which raises the drop chance (card_roll in
 * promo.ds). Unknown lines are ignored while reading and the write is atomic, a
 * temporary file plus rename.
 */
#define PROMO_FILE "promo.dat"
#define PROMO_MAX 24

typedef struct {
    DSMutex lock;
    int loaded;
    int used;
    int streak;
    int card_found;
} Promo;
static Promo pr = { .lock = DS_MUTEX_INIT };
static void pr_lock(void) { ds_mutex_lock(pr.lock); }
static void pr_unlock(void) { ds_mutex_unlock(pr.lock); }
static char s_promo_ret[PROMO_MAX];

/* FNV-1a with a salt, so the code is stable across builds */
static unsigned long long promo_hash(const char *nick, unsigned long long basis) {
    const unsigned long long prime = 1099511628211ull;
    unsigned long long h = basis;
    for (const char *p = nick; *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        h ^= (unsigned char)c;
        h *= prime;
    }
    return h;
}

/* Current format: 3 Latin letters plus 1 digit, such as ABC7, derived from the nick. */
static void promo_code_for_nick(const char *nick, char *out, size_t cap) {
    out[0] = 0;
    if (!nick || !*nick || cap < 8) return;
    if (strlen(nick) > LOGIN_NICK_MAX) return;
    unsigned long long h = promo_hash(nick, 1469598103934665603ull);
    unsigned long long h2 = promo_hash(nick, 0xCBF29CE484222325ull);
    char l1 = 'A' + (h % 26);
    char l2 = 'A' + ((h >> 5) % 26);
    char l3 = 'A' + ((h >> 10) % 26);
    char d = '0' + (h2 % 10);
    /* The digit position is shuffled by the hash, so it is not always last */
    int pos = h % 4;
    char code[5];
    code[4] = 0;
    for (int i = 0; i < 4; i++) code[i] = 'A';
    if (pos == 0) { code[0] = d; code[1] = l1; code[2] = l2; code[3] = l3; }
    else if (pos == 1) { code[0] = l1; code[1] = d; code[2] = l2; code[3] = l3; }
    else if (pos == 2) { code[0] = l1; code[1] = l2; code[2] = d; code[3] = l3; }
    else { code[0] = l1; code[1] = l2; code[2] = l3; code[3] = d; }
    snprintf(out, cap, "%s", code);
}

/* The personal code of the current player, empty when not logged in. */
const char *net_promo_code(void) {
    char nick[LOGIN_NICK_MAX + 1] = "";
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    s_promo_ret[0] = 0;
    promo_code_for_nick(nick, s_promo_ret, sizeof(s_promo_ret));
    return s_promo_ret;
}

static void promo_read(void) {
    char path[320], line[64];
    FILE *f;
    int used = 0, streak = 0, card_found = 0;
    pr_lock();
    if (pr.loaded) { pr_unlock(); return; }
    pr_unlock();
    data_file_path(path, sizeof(path), PROMO_FILE);
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            int v = 0;
            if (sscanf(line, "used %d", &v) == 1) { used = v ? 1 : 0; continue; }
            if (sscanf(line, "streak %d", &v) == 1) { streak = v < 0 ? 0 : v; continue; }
            if (sscanf(line, "card_found %d", &v) == 1) { card_found = v ? 1 : 0; continue; }
            if (sscanf(line, "card %d", &v) == 1) { card_found = v ? 1 : 0; continue; }
        }
        fclose(f);
    }
    pr_lock();
    pr.used = used;
    pr.streak = streak;
    pr.card_found = card_found;
    pr.loaded = 1;
    pr_unlock();
}

static void promo_write(void) {
    char path[320], text[128];
    int used = 0, streak = 0, card_found = 0;
    promo_read();
    pr_lock();
    used = pr.used; streak = pr.streak; card_found = pr.card_found;
    pr_unlock();
    data_file_path(path, sizeof(path), PROMO_FILE);
    snprintf(text, sizeof(text), "used %d\nstreak %d\ncard_found %d\n", used, streak, card_found);
    file_write_atomic(path, text);
}

double net_promo_card_found(void) {
    double v;
    promo_read();
    pr_lock(); v = (double)pr.card_found; pr_unlock();
    return v;
}

void net_promo_mark_card_found(void) {
    promo_read();
    pr_lock();
    pr.card_found = 1;
    pr_unlock();
    promo_write();
}

double net_promo_used(void) {
    double v;
    promo_read();
    pr_lock(); v = (double)pr.used; pr_unlock();
    return v;
}

/* How many solo matches in a row went without a card, used by card_roll. */
double net_promo_streak(void) {
    double v;
    promo_read();
    pr_lock(); v = (double)pr.streak; pr_unlock();
    return v;
}

/* A match ended without a card, so the next chance is higher. */
void net_promo_bump_streak(void) {
    promo_read();
    pr_lock();
    pr.streak += 1;
    pr_unlock();
    promo_write();
}

/* A card dropped, so the counter resets. */
void net_promo_reset_streak(void) {
    promo_read();
    pr_lock();
    pr.streak = 0;
    pr_unlock();
    promo_write();
}

/* Activates the promo code: locally and through a PATCH into the cloud. */
void net_promo_mark_used(void) {
    char nick[LOGIN_NICK_MAX + 1] = "";
    promo_read();
    pr_lock();
    pr.used = 1;
    pr_unlock();
    promo_write();
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    if (!nick[0] || !net.base[0]) return;
    char url[URL], body[BODY];
    snprintf(url, sizeof(url), "%s/users/%s.json", net.base, nick);
    snprintf(body, sizeof(body), "{\"promo_used\":1}");
    http_patch_async(url, body);
    if (net_log_ok()) LOG("promo marked used for %s", nick);
}

/* Writes the personal code into the cloud profile, idempotently: when a card is
 * collected and at login if the profile has no code yet. */
void net_promo_register(void) {
    char nick[LOGIN_NICK_MAX + 1] = "";
    char code[PROMO_MAX] = "";
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    if (!nick[0] || !net.base[0]) return;
    promo_code_for_nick(nick, code, sizeof(code));
    char url[URL], body[BODY];
    snprintf(url, sizeof(url), "%s/users/%s.json", net.base, nick);
    snprintf(body, sizeof(body), "{\"promo\":\"%s\"}", code);
    http_patch_async(url, body);
    if (net_log_ok()) LOG("promo registered for %s: %s", nick, code);
}

/* After a cloud profile load (profile_apply.inc): a missing code is appended, and
 * promo_used=1 in the cloud stops the code from being activated again on a clean
 * device, where the local file may be gone. */
int promo_sync_with_cloud(const char *resp) {
    char stored[PROMO_MAX] = "";
    int changed = 0;
    int used = (int)num(resp, "promo_used", -1);
    strv(resp, "promo", stored, sizeof(stored));
    char nick[LOGIN_NICK_MAX + 1] = "";
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    if (!nick[0]) return 0;
    if (!stored[0]) {
        net_promo_register();
        changed = 1;
    }
    if (used > 0) {
        promo_read();
        pr_lock();
        int was = pr.used;
        pr.used = 1;
        pr_unlock();
        if (!was) {
            promo_write();
            changed = 1;
        }
    }
    return changed;
}
