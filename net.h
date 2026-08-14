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
#define NET_AUTH_ERROR 5
#define NET_ACCOUNT_EMPTY 0
#define NET_ACCOUNT_CHECKING 1
#define NET_ACCOUNT_READY 2
#define NET_ACCOUNT_WRONG_PASSWORD 3
#define NET_ACCOUNT_INVALID 4
#define NET_ACCOUNT_ERROR 5
#ifdef __ANDROID__
void net_set_java_vm(JavaVM *vm);
#endif
void net_set_storage_path(const char *path);
void net_account_configure(const char *url, const char *room);
void net_account_login(const char *nickname, const char *password);
double net_account_status(void);
const char *net_account_nickname(void);
const char *net_account_error(void);
void net_connect(const char *url, const char *room);
void net_disconnect(void);
void net_publish(double x, double y, double a, double hp, double alive);
void net_publish_bullet(double x, double y, double dx, double dy, double active, double shot, double tr);
double net_status(void);
double net_slot(void);
double net_count(void);
double net_player_online(double slot);
double net_player_x(double slot);
double net_player_y(double slot);
double net_player_angle(double slot);
double net_player_hp(double slot);
double net_player_alive(double slot);
const char *net_player_nickname(double slot);
double net_player_bullet_active(double slot);
double net_player_bullet_x(double slot);
double net_player_bullet_y(double slot);
double net_player_bullet_dx(double slot);
double net_player_bullet_dy(double slot);
double net_player_bullet_shot(double slot);
double net_player_bullet_tr(double slot);
void net_chat_send(const char *text);
double net_chat_count(void);
const char *net_chat_text(double idx);
const char *net_chat_uid(double idx);
const char *net_chat_nickname(double idx);
double net_chat_time(double idx);
#endif
