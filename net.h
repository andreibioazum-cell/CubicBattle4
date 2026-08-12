#ifndef NET_H
#define NET_H

/* Онлайн-слой Gig1.0: Firebase Realtime Database поверх обычного REST.
 *
 * В комнате может быть до NET_MAX_SLOTS игроков. Свой слот клиент занимает
 * сам при подключении, а все остальные слоты просто читает и интерпретирует
 * как противников. Игра не блокируется экраном ожидания: как только слот
 * занят, локальный игрок уже может двигаться, стрелять и принимать пули тех,
 * кто подключился к этой же комнате.
 *
 *   rooms/<комната>/players/<слот>   uid, x, y, angle, hp, alive, seq
 *   rooms/<комната>/bullets/<слот>   x, y, dx, dy, active, shot
 *
 * Вся сеть живёт в отдельном потоке: игровой цикл никогда не ждёт ответа
 * сервера, он лишь кладёт своё состояние в net_publish* и забирает чужое
 * из net_player_*. Скрипт видит эти функции как встроенные.
 */

#define NET_MAX_SLOTS 8

/* Состояние соединения — им удобно рисовать надпись в углу экрана. */
#define NET_OFFLINE     0   /* поток не запущен */
#define NET_CONNECTING  1   /* ищем свободный слот в комнате */
#define NET_WAITING     2   /* слот занят; оставлено для совместимости */
#define NET_PLAYING     3   /* соединение с базой активно */
#define NET_ERROR       4   /* сеть недоступна, идут повторные попытки */

/* Подключение к комнате. base_url — корень базы, например
 * "https://cubic-battleserver-19ae2-default-rtdb.firebaseio.com".
 * Повторный вызов при уже поднятом соединении игнорируется. */
void net_connect(const char *base_url, const char *room);

/* Отключение: поток останавливается, свой слот в базе освобождается. */
void net_disconnect(void);

/* Своё состояние. Вызывайте каждый кадр — в сеть уйдёт последнее. */
void net_publish(double x, double y, double angle, double hp, double alive);
void net_publish_bullet(double x, double y, double dx, double dy,
                        double active, double shot);

/* Состояние соединения, свой слот (-1, пока слот не занят) и число живых
 * игроков в комнате, включая локального. */
double net_status(void);
double net_online(void);
double net_slot(void);
double net_player_count(void);

/* Универсальный доступ к любому слоту. slot может быть от 0 до
 * NET_MAX_SLOTS-1. Для локального слота net_player_online() возвращает 1
 * сразу после подключения. */
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

/* Совместимость со старым скриптом на одного соперника: это состояние
 * первого онлайн-игрока, который занимает не наш слот. */
double net_peer_online(void);
double net_peer_x(void);
double net_peer_y(void);
double net_peer_angle(void);
double net_peer_hp(void);
double net_peer_alive(void);
double net_peer_bullet_active(void);
double net_peer_bullet_x(void);
double net_peer_bullet_y(void);
double net_peer_bullet_dx(void);
double net_peer_bullet_dy(void);
double net_peer_bullet_shot(void);

/* --- Служебное, вызывает хост, а не скрипт --- */

#ifdef __ANDROID__
#include <jni.h>
/* Android-бэкенд ходит в сеть через java.net.HttpURLConnection, поэтому
 * ему нужна JavaVM. Вызывается один раз из android_main. */
void net_set_java_vm(JavaVM *vm);
#endif

/* Полная остановка (перезапуск скрипта, закрытие окна). */
void net_shutdown(void);

#endif
