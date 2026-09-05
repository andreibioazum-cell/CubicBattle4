}
void net_publish_punch(double x, double y, double dx, double dy, double punch) {
    lock();
    net.me.punch_x=x; net.me.punch_y=y;
    net.me.punch_dx=dx; net.me.punch_dy=dy; net.me.punch=punch;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
void net_publish_snow(double x, double y, double dx, double dy, double snow) {
    lock();
    net.me.snow_x=x; net.me.snow_y=y;
    net.me.snow_dx=dx; net.me.snow_dy=dy; net.me.snow=snow;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
void net_publish_station(double x, double y, double hp, double counter) {
    lock();
    net.me.station_x=x; net.me.station_y=y; net.me.station_hp=hp; net.me.station=counter;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
void net_publish_universe(double x, double y, double counter) {
    lock();
    net.me.universe_x=x; net.me.universe_y=y; net.me.universe=counter;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
void net_chat_send(const char *text){
    if(!net.started || !text || !*text) return;
    if(!net.run) return;
    char url[URL], body[BODY*2], esc[CHAT_TEXT_MAX*2], enick[64];
    const char *nick;
    json_escape(text, esc, sizeof(esc));
    lock(); nick = net.me.nick[0] ? net.me.nick : net.uid; json_escape(nick, enick, sizeof(enick)); unlock();
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json",net.base,net.room);
    snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"text\":\"%s\"}",net.uid,enick,esc);
    http_post_async(url, body);
    LOG("chat send %s",text);
}
void net_chat_trim(double keep){
    int k=(int)keep;
    char extra[CHAT_MAX][28];
    int nextra=0, drop, i;
    if(k<1) k=1;
    if(k>CHAT_MAX) k=CHAT_MAX;
    lock();
    chat_keep=k;
    if(net.chat_count>k){
        drop=net.chat_count-k;
        for(i=0;i<drop;i++){
            if(net.chats[i].key[0]){
                strncpy(extra[nextra], net.chats[i].key, sizeof(extra[0])-1);
                extra[nextra][sizeof(extra[0])-1]=0;
                nextra++;
            }
        }
        memmove(net.chats, net.chats+drop, (size_t)k*sizeof(net.chats[0]));
        net.chat_count=k;
        if(k<CHAT_MAX) memset(net.chats+k, 0, (size_t)(CHAT_MAX-k)*sizeof(net.chats[0]));
    }
    unlock();
    for(i=0;i<nextra;i++) chat_delete_key_async(extra[i]);
}
double net_chat_count(void){ double v; lock(); v=net.chat_count; unlock(); return v; }
const char* net_chat_text(double idx){
    int i=(int)idx;
    lock();
    if(i>=0 && i<net.chat_count && net.chats[i].valid){
        strncpy(s_chat_text_ret, net.chats[i].text, sizeof(s_chat_text_ret)-1);
        s_chat_text_ret[sizeof(s_chat_text_ret)-1]='\0';
    }else{
        s_chat_text_ret[0]='\0';
    }
    unlock();
    return s_chat_text_ret;
}
const char* net_chat_uid(double idx){
    int i=(int)idx;
    lock();
    if(i>=0 && i<net.chat_count && net.chats[i].valid){
        const char *src = net.chats[i].nick[0] ? net.chats[i].nick : net.chats[i].uid;
        strncpy(s_chat_uid_ret, src, sizeof(s_chat_uid_ret)-1);
        s_chat_uid_ret[sizeof(s_chat_uid_ret)-1]='\0';
    }else{
        s_chat_uid_ret[0]='\0';
    }
    unlock();
    return s_chat_uid_ret;
}
static int sidx(double slot){ int i=(int)slot; return i>=0&&i<NET_SLOTS?i:-1; }
double net_status(void){ double v; lock(); v=net.status; unlock(); return v; }
double net_slot(void){ double v; lock(); v=net.slot; unlock(); return v; }
double net_event(void){ double v; lock(); v=net.event; unlock(); return v; }

/* Баннер админа: последний баннер, прочитанный из облака. Скрипт сравнивает
 * ts с тем, что уже показывал, и решает, показывать ли текст заново. */
double net_banner_ts(void){ double v; lock(); v=(double)net.banner_ts; unlock(); return v; }
const char *net_banner_text(void){
    lock();
    snprintf(s_banner_text_ret,sizeof(s_banner_text_ret),"%s",net.banner_text);
    unlock();
    return s_banner_text_ret;
}
const char *net_banner_color(void){
    lock();
    snprintf(s_banner_color_ret,sizeof(s_banner_color_ret),"%s",net.banner_color);
    unlock();
    return s_banner_color_ret;
}
double net_count(void){ double v; lock(); v=net.count; unlock(); return v; }
const char* net_player_nick(double slot){
    int i=sidx(slot);
    lock();
    if(i>=0 && net.players[i].nick[0]){
        strncpy(s_nick_ret, net.players[i].nick, sizeof(s_nick_ret)-1);
        s_nick_ret[sizeof(s_nick_ret)-1]='\0';
    }else{
        s_nick_ret[0]='\0';
    }
    unlock();
    return s_nick_ret;
}
#define READER(name, field) double name(double slot){ int i=sidx(slot); double v=0; if(i>=0){lock();v=field;unlock();} return v; }
READER(net_player_online, net.players[i].online?1:0)
READER(net_player_x, net.players[i].x)
READER(net_player_y, net.players[i].y)
READER(net_player_angle, net.players[i].a)
READER(net_player_hp, net.players[i].hp)
READER(net_player_alive, net.players[i].alive)
READER(net_player_punch_x, net.players[i].punch_x)
READER(net_player_punch_y, net.players[i].punch_y)
READER(net_player_punch_dx, net.players[i].punch_dx)
READER(net_player_punch_dy, net.players[i].punch_dy)
READER(net_player_punch, net.players[i].punch)
READER(net_player_snow_x, net.players[i].snow_x)
READER(net_player_snow_y, net.players[i].snow_y)
READER(net_player_snow_dx, net.players[i].snow_dx)
READER(net_player_snow_dy, net.players[i].snow_dy)
READER(net_player_snow, net.players[i].snow)
READER(net_player_station_x, net.players[i].station_x)
READER(net_player_station_y, net.players[i].station_y)
READER(net_player_station_hp, net.players[i].station_hp)
READER(net_player_station, net.players[i].station)
READER(net_player_universe_x, net.players[i].universe_x)
READER(net_player_universe_y, net.players[i].universe_y)
READER(net_player_universe, net.players[i].universe)
READER(net_player_class, net.players[i].cls)
#undef READER
double net_player_level(double slot) {
    int i=sidx(slot); double v=0;
    if(i>=0){ lock(); v=net.players[i].level; unlock(); }
    if(v<0) v=0;
    if(v>LEVEL_MAX) v=LEVEL_MAX;
    return v;
}
/* Скин бойца в снимке комнаты. Старые клиенты поле не присылают — тогда оно
 * читается как 0 (обычный скин), поэтому смешанные версии в онлайне работают. */
double net_player_skin(double slot) {
    int i=sidx(slot); double v=0;
    if(i>=0){ lock(); v=net.players[i].skin; unlock(); }
    if(v<0) v=0;
    if(v>SKIN_MAX) v=SKIN_MAX;
    return v;
}
