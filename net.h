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
/* Ключ Firebase (API key, ограниченный пакетом com.cb4): включает защищённый
 * режим — все запросы к базе подписываются токеном Firebase Auth. */
void net_set_firebase_key(const char *key);
void net_autologin(const char *url);
double net_auth(const char *url, const char *nick, const char *pass);
double net_set_nick(const char *nick);
void net_logout(void);
double net_login_status(void);
const char *net_login_nick(void);
const char *net_login_pass(void);

void net_leaderboard_fetch(const char *url);
double net_leaderboard_status(void);
double net_leaderboard_count(void);
const char *net_leaderboard_nick(double idx);
double net_leaderboard_cups(double idx);

void net_publish(double x, double y, double angle, double hp, double alive);
void net_publish_punch(double x, double y, double dx, double dy, double punch);
void net_publish_snow(double x, double y, double dx, double dy, double snow);
void net_publish_station(double x, double y, double hp, double counter);
void net_publish_universe(double x, double y, double counter);
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
double net_player_station(double slot);
double net_player_universe_x(double slot);
double net_player_universe_y(double slot);
double net_player_universe(double slot);
double net_player_class(double slot);
double net_player_level(double slot);
void net_set_level(double level);

/* Прогресс без праймов */
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
/* Хэллоуинский батл пасс и скины Азума. */
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

/* Достижения профиля: единый битмаск в achievements.dat. ACH_FLAG_* ниже —
 * код отдельных наград. Добавление новой — это новый бит, старые файлы
 * читаются как есть.
 *  WELCOME        — первый запуск игры;
 *  FIRST_WIN      — первая победа;
 *  FIRST_BUY      — первая покупка класса;
 *  ALL_CHARACTERS — собраны все открываемые классы. */
#define ACH_FLAG_WELCOME         (1u << 0)
#define ACH_FLAG_FIRST_WIN       (1u << 1)
#define ACH_FLAG_FIRST_BUY       (1u << 2)
#define ACH_FLAG_ALL_CHARACTERS  (1u << 3)
double net_load_achievement_flags(void);
void net_save_achievement_flags(double flags);
double net_has_achievement_flag(double flag);
void net_mark_achievement_flag(double flag);

/* Бан система */
double net_banned(void);
double net_is_banned(const char *nick);
void net_ban_set(const char *nick, double banned);
double net_chat_is_ban(const char *msg);
double net_chat_is_unban(const char *msg);
const char* net_chat_ban_target(const char *msg);
const char* net_chat_unban_target(const char *msg);

/* Команда админа "text <сообщение> <цвет>" — баннер сверху экрана у всех. */
double net_chat_is_text_cmd(const char *msg);
const char* net_chat_text_cmd_text(const char *msg);
const char* net_chat_text_cmd_color(const char *msg);
void net_banner_send(const char *text, const char *color);
double net_banner_ts(void);
const char *net_banner_text(void);
const char *net_banner_color(void);

/* Чат */
void net_chat_send(const char *text);
void net_chat_trim(double keep);
double net_chat_count(void);
const char *net_chat_text(double idx);
const char *net_chat_uid(double idx);

#endif
