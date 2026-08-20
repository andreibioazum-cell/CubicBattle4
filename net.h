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

#ifdef __ANDROID__
void net_set_java_vm(JavaVM *vm);
#endif

/* Подключение и локальный ник. Регистрации и паролей нет. */
void net_connect(const char *url, const char *room);
void net_disconnect(void);
void net_set_data_path(const char *path);
void net_autologin(const char *url);
double net_set_nick(const char *nick);
double net_login_status(void);
const char *net_login_nick(void);

/* Состояние игроков. Удар — постоянное событие со счётчиком punch:
 * если счётчик изменился, удар нельзя потерять между двумя опросами сети. */
void net_publish(double x, double y, double angle, double hp, double alive);
void net_publish_punch(double x, double y, double dx, double dy, double punch);
void net_set_class(double cls);
double net_status(void);
double net_slot(void);
double net_count(void);
double net_event(void); /* 1 — ивент «диско» включён, 0 — ивента нет */
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
double net_player_class(double slot);

/* Локальный прогресс: кубки, выбранный класс и купленный Азум. */
double net_load_cups(void);
double net_load_class(void);
double net_load_azum(void);
void net_save_progress(double cups, double cls, double azum);

/* Чат читается реже боевого состояния, поэтому не тормозит движение. */
void net_chat_send(const char *text);
void net_chat_trim(double keep); /* оставить последние keep, остальное удалить */
double net_chat_count(void);
const char *net_chat_text(double idx);
const char *net_chat_uid(double idx);

#endif
