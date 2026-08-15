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
/* login states (net_login_status) */
#define NET_LOGIN_IDLE 0
#define NET_LOGIN_BUSY 1
#define NET_LOGIN_OK 2
#define NET_LOGIN_WRONG_PWD 3
#define NET_LOGIN_ERROR 4
#define NET_LOGIN_INVALID 5
#define NET_LOGIN_EXISTS 6
#ifdef __ANDROID__
void net_set_java_vm(JavaVM *vm);
#endif
void net_connect(const char *url, const char *room);
void net_disconnect(void);
void net_set_data_path(const char *path);
void net_autologin(const char *url);
void net_login(const char *url, const char *nick, const char *pwd);
void net_register(const char *url, const char *nick, const char *pwd);
void net_logout(void);
double net_login_status(void);
const char *net_login_nick(void);
const char *net_player_nick(double slot);
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
double net_chat_time(double idx);
#endif
