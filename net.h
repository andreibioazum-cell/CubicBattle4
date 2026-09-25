#ifndef NET_H
#define NET_H
#ifdef __ANDROID__
#include <jni.h>
#endif

#define NET_SLOTS 4
#define NET_OFFLINE 0
#define NET_CONNECTING 1
#define NET_PLAYING 3
#define NET_ERROR 4
#define NET_LOGIN_IDLE 0
#define NET_LOGIN_OK 2
#define NET_LOGIN_BAD_NICK 5
#define NET_LOGIN_WRONG_PASS 6
#define NET_LOGIN_BAD_PASS 7

#ifdef __ANDROID__
void net_set_java_vm(JavaVM *vm);
#endif

void net_connect(const char *url, const char *room);
void net_disconnect(void);
void net_set_data_path(const char *path);
/* Firebase API key, restricted to the com.cb4 package: it turns on the protected
 * mode, where every database request is signed with a Firebase Auth token. */
void net_set_firebase_key(const char *key);
void net_autologin(const char *url);
double net_auth(const char *url, const char *nick, const char *pass);
double net_set_nick(const char *nick);
void net_logout(void);
double net_login_status(void);
const char *net_login_nick(void);
const char *net_login_pass(void);

void net_publish(double x, double y, double angle, double hp, double alive);
void net_publish_punch(double x, double y, double dx, double dy, double punch);
void net_publish_snow(double x, double y, double dx, double dy, double snow);
/* ebuC turrets: up to three live turrets, each with its own position and HP.
 * count is how many are alive right now (0..3), and station1_ to station3_ hold
 * turrets 1, 2 and 3 in the order they were placed. */
void net_publish_turrets(double x1, double y1, double hp1,
                         double x2, double y2, double hp2,
                         double x3, double y3, double hp3,
                         double count);
/* Azum dash: the start point, the direction and a counter. The counter changes on
 * every dash, which is how receivers spot that one started. */
void net_publish_dash(double x, double y, double dx, double dy, double dash);
void net_publish_universe(double x, double y, double counter);
/* ebuC turret counter hit: an event counter for "my turret was hit". A client that
 * sees a new counter on the remote side takes a little damage on its own fighter,
 * since each client is authoritative over its own fighter (net_player_thud). */
void net_publish_thud(double counter);
void net_set_class(double cls);
double net_status(void);
double net_slot(void);
double net_count(void);
double net_event(void);
void net_event_set(double mode);
double net_player_online(double slot);
double net_player_x(double slot);
double net_player_y(double slot);
double net_player_angle(double slot);
double net_player_hp(double slot);
double net_player_alive(double slot);
const char *net_player_nick(double slot);
double net_player_punch_x(double slot);
double net_player_punch_y(double slot);
double net_player_punch_dx(double slot);
double net_player_punch_dy(double slot);
double net_player_punch(double slot);
double net_player_snow_x(double slot);
double net_player_snow_y(double slot);
double net_player_snow_dx(double slot);
double net_player_snow_dy(double slot);
double net_player_snow(double slot);
double net_player_station_x(double slot);
double net_player_station_y(double slot);
double net_player_station_hp(double slot);
double net_player_station2_x(double slot);
double net_player_station2_y(double slot);
double net_player_station2_hp(double slot);
double net_player_station3_x(double slot);
double net_player_station3_y(double slot);
double net_player_station3_hp(double slot);
/* How many turrets the player has alive (0..3). */
double net_player_station(double slot);
double net_player_universe_x(double slot);
double net_player_universe_y(double slot);
double net_player_universe(double slot);
/* Player turret counter hit counter. Old clients send no such field at all, which
 * keeps mixed versions working online. */
double net_player_thud(double slot);
double net_player_dash(double slot);
double net_player_dash_x(double slot);
double net_player_dash_y(double slot);
double net_player_dash_dx(double slot);
double net_player_dash_dy(double slot);
double net_player_class(double slot);
double net_player_level(double slot);
void net_set_level(double level);
/* Fighter skin, 0 for ordinary and 1 for the Azum zombie: it travels in the room
 * snapshot so remotes see it. */
void net_set_skin(double skin);
double net_player_skin(double slot);

/* Progress without primes */
double net_load_cups(void);
double net_load_candies(void);
double net_load_class(void);
double net_load_azum(void);
double net_load_santa(void);
double net_load_ebuc(void);
double net_load_level(void);
double net_load_levels_unlocked(void);
double net_load_ordinary_level(void);
double net_load_ordinary_levels_unlocked(void);
double net_load_azum_level(void);
double net_load_azum_levels_unlocked(void);
double net_load_santa_level(void);
double net_load_santa_levels_unlocked(void);
double net_load_ebuc_level(void);
double net_load_ebuc_levels_unlocked(void);
/* Azum skins. The bp_level field of the removed battle pass stays in the same
 * positional spot of the progress record so old saves still read the same way,
 * but the game no longer uses it. */
double net_load_bp_level(void);
double net_load_azum_skin(void);
void net_save_progress(double cups, double candies, double cls, double azum, double santa, double ebuc,
                       double level, double levels_unlocked);
void net_save_progress_all(double cups, double candies, double cls, double azum, double santa, double ebuc,
                           double level, double levels_unlocked,
                           double ordinary_level, double ordinary_levels_unlocked,
                           double azum_level, double azum_levels_unlocked,
                           double santa_level, double santa_levels_unlocked,
                           double ebuc_level, double ebuc_levels_unlocked,
                           double bp_level, double azum_skin);

/* Profile achievements: one bitmask in achievements.dat, with the ACH_FLAG_*
 * values below as the individual rewards. A new achievement takes a new bit, and
 * old files keep reading as they are:
 *  FIRST_WIN       first win;
 *  FIRST_BUY       first class purchase;
 *  ALL_CHARACTERS  every unlockable class collected;
 *  LEGENDS         25 revives as Azum, which gives the zombie skin.
 * Bit 0 belonged to the removed "hello" achievement; the current code neither sets
 * nor reads it, so old files simply lose that bit. */
#define ACH_FLAG_FIRST_WIN       (1u << 1)
#define ACH_FLAG_FIRST_BUY       (1u << 2)
#define ACH_FLAG_ALL_CHARACTERS  (1u << 3)
#define ACH_FLAG_LEGENDS         (1u << 4)
double net_load_achievement_flags(void);
void net_save_achievement_flags(double flags);
double net_has_achievement_flag(double flag);
void net_mark_achievement_flag(double flag);
/* Azum revive counter, stored as the second number of achievements.dat so it
 * survives a restart without touching the progress.dat format. */
double net_load_azum_revives(void);
void net_save_azum_revives(double revives);

/* Promo codes of the solo match cards. net_promo_new_code makes a fresh random
 * code when a card is picked up (3 letters and 1 digit) and keeps it as the code
 * of the last card, locally and in the cloud profile; net_promo_code returns it.
 * net_promo_check says whether an entered code is that code, and
 * net_promo_mark_used takes the reward, one per account, locally and in the
 * cloud. The implementation is native/net/promo.inc. */
const char *net_promo_code(void);
const char *net_promo_new_code(void);
double net_promo_check(const char *code);
double net_promo_used(void);
void net_promo_mark_used(void);
double net_load_playtime(void);
void net_save_playtime(double seconds);
void net_add_playtime(double delta);

/* Quests: the state survives a restart (native/net/quests.inc), and
 * net_quest_now gives the current epoch so the countdown keeps running while the
 * game is closed. */
double net_quest_now(void);
void net_save_quest_state(double t0, double p0, double n0, double x0,
                          double t1, double p1, double n1, double x1,
                          double t2, double p2, double n2, double x2);
double net_load_quest_state(double slot, double field);
double net_quest_has_state(void);

void net_leaderboard_fetch(const char *url);
double net_leaderboard_status(void);
double net_leaderboard_count(void);
const char *net_leaderboard_nick(double idx);
double net_leaderboard_cups(double idx);

/* Player settings: language (0 English, 1 Russian, as in ui/locale_core.ds),
 * hitboxes (1 means visible), music volume (0..100, 70 by default), the winter
 * theme (1 for the snowy arena and the snowfall, the default) and the battle
 * frame counter (0, off, by default). The fps cap and the upscale were removed
 * at the player's request, so those settings are gone too. They live in
 * settings.dat on the device and in the /users/<nick> profile in the cloud,
 * saving to both like progress does. The implementation is
 * settings_storage.inc. */
double net_load_language(void);
double net_load_hitboxes(void);
void net_save_settings(double language, double hitboxes);
double net_load_music_volume(void);
void net_save_music_volume(double volume);
double net_load_winter_theme(void);
void net_save_winter_theme(double on);
double net_load_fps_meter(void);
void net_save_fps_meter(double on);

/* Epilepsy warning consent (settings_storage.inc): the timestamp of the consent
 * button press, kept on the device only. */
void settings_mark_legal(void);
double settings_legal_ts(void);

/* Ban system */
double net_banned(void);
double net_is_banned(const char *nick);
void net_ban_set(const char *nick, double banned);
double net_chat_is_ban(const char *msg);
double net_chat_is_unban(const char *msg);
const char* net_chat_ban_target(const char *msg);
const char* net_chat_unban_target(const char *msg);

/* The admin command "text <message> <colour>" shows a banner on every screen. */
double net_chat_is_text_cmd(const char *msg);
const char* net_chat_text_cmd_text(const char *msg);
const char* net_chat_text_cmd_color(const char *msg);
void net_banner_send(const char *text, const char *color);
double net_banner_ts(void);
const char *net_banner_text(void);
const char *net_banner_color(void);

/* Chat */
void net_chat_send(const char *text);
void net_chat_trim(double keep);
double net_chat_count(void);
const char *net_chat_text(double idx);
const char *net_chat_uid(double idx);
/* Server key of a message: unique per message, unlike the author uid, and the
 * script uses it to tell which messages already have bubbles. */
const char *net_chat_key(double idx);

#endif
