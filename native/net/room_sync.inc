/* Последнее успешно отправленное состояние: анти-флуд (см. push_state).
 * Сбрасывается в net_connect, чтобы первая отправка новой сессии не сравнивалась
 * с состоянием прошлой. */
static Actor ps_last;
static long long ps_last_put = 0;
static int ps_have_last = 0;
static int push_state(void) {
    Actor a; int slot; unsigned long seq; char url[URL], body[BODY], enick[64];
    lock(); a=net.me; slot=net.slot; seq=++net.seq; unlock();
    if(slot<0) return 0;
    /* Стоим на месте и свежий heartbeat уже был — базу не дёргаем. seq обязан
     * обновляться хотя бы раз в HEARTBEAT_TICK: иначе другие игроки через
     * TIMEOUT сочтут слот мёртвым и заберут его. */
    if (ps_have_last && actor_same(&a,&ps_last) && now_ms()-ps_last_put < HEARTBEAT_TICK) return 1;
    json_escape(a.nick,enick,sizeof(enick));
    snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot);
    snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"x\":%.5f,\"y\":%.5f,\"angle\":%.5f,\"hp\":%.0f,\"alive\":%.0f,\"seq\":%lu,\"px\":%.5f,\"py\":%.5f,\"pdx\":%.5f,\"pdy\":%.5f,\"punch\":%.0f,\"sx\":%.5f,\"sy\":%.5f,\"sdx\":%.5f,\"sdy\":%.5f,\"snow\":%.0f,\"stx\":%.5f,\"sty\":%.5f,\"sthp\":%.0f,\"station\":%.0f,\"uzx\":%.5f,\"uzy\":%.5f,\"universe\":%.0f,\"cls\":%.0f,\"level\":%.0f,\"skin\":%.0f}",
        net.uid,enick,clamp01(a.x),clamp01(a.y),safe(a.a),safe(a.hp),safe(a.alive),seq,
        clamp01(a.punch_x),clamp01(a.punch_y),clamp11(a.punch_dx),clamp11(a.punch_dy),safe(a.punch),
        clamp01(a.snow_x),clamp01(a.snow_y),clamp11(a.snow_dx),clamp11(a.snow_dy),safe(a.snow),clamp01(a.station_x),clamp01(a.station_y),safe(a.station_hp),safe(a.station),clamp01(a.universe_x),clamp01(a.universe_y),safe(a.universe),safe(a.cls),safe(a.level),safe(a.skin));
    int c = fb_http("PUT",url,body,NULL,0);
    if (c != 200 && net_log_ok()) LOGERR("push state: HTTP %d", c);
    if (c == 200) { ps_last = a; ps_last_put = now_ms(); ps_have_last = 1; }
    return c == 200;
}
static int pull_state(char *resp,size_t cap) {
    char url[URL];
    snprintf(url,sizeof(url),"%s/rooms/%s/players.json",net.base,net.room);
    int c = fb_http("GET",url,NULL,resp,cap);
    if (c != 200 && net_log_ok()) LOGERR("pull state: HTTP %d", c);
    return c == 200;
}
static int pull_event(char *resp,size_t cap) {
    char url[URL];
    snprintf(url,sizeof(url),"%s/event.json",net.base);
    int c = fb_http("GET",url,NULL,resp,cap);
    if (c != 200 && net_log_ok()) LOGERR("pull event: HTTP %d", c);
    return c == 200;
}
static void parse_and_store_banner(const char *json) {
    char text[CHAT_TEXT_MAX], color[BANNER_COLOR_MAX];
    long long ts;
    if (!json || !*json || !strncmp(json,"null",4)) return;
    ts = (long long)num(json,"ts",0);
    if (ts <= 0) return;
    strv(json,"text",text,sizeof(text));
    strv(json,"color",color,sizeof(color));
    if (!text[0]) return;
    lock();
    snprintf(net.banner_text,sizeof(net.banner_text),"%s",text);
    snprintf(net.banner_color,sizeof(net.banner_color),"%s",color[0] ? color : "white");
    net.banner_ts = ts;
    unlock();
}
static int pull_banner(char *resp,size_t cap) {
    char url[URL];
    snprintf(url,sizeof(url),"%s/banner.json",net.base);
    return fb_http("GET",url,NULL,resp,cap)==200;
}

static void release_slot(void) {
    int slot; char url[URL]; lock(); slot=net.slot; net.slot=-1; unlock(); if(slot<0)return;
    snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot); fb_http("DELETE",url,NULL,NULL,0);
}
static int claim_slot(void) {
    char resp[RESP],url[URL],body[BODY],uid[24],etag[96]; long long t=now_ms(); int slot;
    for(slot=0;slot<NET_SLOTS;slot++) {
        unsigned long seq; int claim=0,code;
        if (!net.run) return -1;
        snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot);
        code=fb_http_ex("GET",url,NULL,resp,sizeof(resp),"X-Firebase-ETag","true",etag,sizeof(etag));
        if(code!=200) { if(net_log_ok()) LOGERR("claim slot %d: HTTP %d", slot, code); continue; }
        strv(resp,"uid",uid,sizeof(uid));
        if(uid[0]&&!strcmp(uid,net.uid))return slot;
        seq=(unsigned long)num(resp,"seq",0);
        if(!uid[0])claim=1;
        else if(etag[0]&&(strcmp(uid,claim_seen_uid[slot])||seq!=claim_seen_seq[slot])) { snprintf(claim_seen_uid[slot],sizeof(claim_seen_uid[slot]),"%s",uid); claim_seen_seq[slot]=seq; claim_seen_at[slot]=t; }
        else if(etag[0]&&t-claim_seen_at[slot]>=STALE)claim=1;
        if(!claim)continue;
        if(!etag[0]) {
            if(net_log_ok()) LOG("claim slot %d: no ETag", slot);
        }
        {
            char enick[64];
            json_escape(net.me.nick,enick,sizeof(enick));
            snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"x\":0,\"y\":0,\"angle\":0,\"hp\":0,\"alive\":0,\"seq\":0,\"px\":0,\"py\":0,\"pdx\":0,\"pdy\":0,\"punch\":0,\"sx\":0,\"sy\":0,\"sdx\":0,\"sdy\":0,\"snow\":0,\"stx\":0,\"sty\":0,\"sthp\":0,\"station\":0,\"uzx\":0,\"uzy\":0,\"universe\":0,\"cls\":%.0f,\"level\":%.0f,\"skin\":%.0f}",net.uid,enick,safe(net.me.cls),safe(net.me.level),safe(net.me.skin));
        }
        code=fb_http_ex("PUT",url,body,NULL,0, etag[0] ? "if-match" : NULL, etag[0] ? etag : NULL, NULL, 0);
        if(code==200) { claim_seen_uid[slot][0]=0; claim_seen_seq[slot]=0; claim_seen_at[slot]=0; LOG("slot %d uid %s",slot,net.uid); return slot; }
        if(net_log_ok()) LOGERR("claim slot %d: PUT failed, HTTP %d", slot, code);
    }
    return -1;
}
static void read_players(const char *resp) {
    Actor ps[NET_SLOTS]; long long t=now_ms(); int local,count=0,slot;
    memset(ps,0,sizeof(ps));
    lock(); local=net.slot; unlock();
    for(slot=0;slot<NET_SLOTS;slot++) {
        char bp[24],p[40],uid[24]; unsigned long sq; int online;
        if(slot==local) { lock(); ps[slot]=net.me; ps[slot].online=local>=0; unlock(); if(local>=0)count++; continue; }
        snprintf(bp,sizeof(bp),"%d",slot); snprintf(p,sizeof(p),"%s/uid",bp); strv(resp,p,uid,sizeof(uid));
        if(!uid[0]||!strcmp(uid,net.uid)){ rp_lseq[slot]=0; rp_lch[slot]=0; continue; }
        snprintf(p,sizeof(p),"%s/seq",bp); sq=(unsigned long)num(resp,p,0);
        if(sq!=rp_lseq[slot]){ rp_lseq[slot]=sq; rp_lch[slot]=t; } else if(!rp_lch[slot]) rp_lch[slot]=t;
        online=t-rp_lch[slot]<TIMEOUT; if(!online)continue;
        snprintf(p,sizeof(p),"%s/nick",bp); strv(resp,p,ps[slot].nick,sizeof(ps[slot].nick));
        if(ban_cache_has(ps[slot].nick)) {
            /* Забаненный игрок не показывается и выкидывается из комнаты:
             * так бан действует и на тех, кто зашёл со старой версией. */
            if(t-rp_kicked[slot]>=BAN_KICK_TICK) {
                rp_kicked[slot]=t;
                LOG("slot %d: '%s' is banned, kicking", slot, ps[slot].nick);
                room_slot_delete_async(slot);
            }
            rp_lseq[slot]=0; rp_lch[slot]=0;
            continue;
        }
        snprintf(p,sizeof(p),"%s/x",bp); ps[slot].x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/y",bp); ps[slot].y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/angle",bp); ps[slot].a=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/hp",bp); ps[slot].hp=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/alive",bp); ps[slot].alive=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/px",bp); ps[slot].punch_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/py",bp); ps[slot].punch_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/pdx",bp); ps[slot].punch_dx=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/pdy",bp); ps[slot].punch_dy=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/punch",bp); ps[slot].punch=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/sx",bp); ps[slot].snow_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/sy",bp); ps[slot].snow_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/sdx",bp); ps[slot].snow_dx=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/sdy",bp); ps[slot].snow_dy=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/snow",bp); ps[slot].snow=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/stx",bp); ps[slot].station_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/sty",bp); ps[slot].station_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/sthp",bp); ps[slot].station_hp=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/station",bp); ps[slot].station=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/uzx",bp); ps[slot].universe_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/uzy",bp); ps[slot].universe_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/universe",bp); ps[slot].universe=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/cls",bp); ps[slot].cls=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/level",bp); ps[slot].level=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/skin",bp); ps[slot].skin=num(resp,p,0);
        ps[slot].online=1; count++;
    }
    lock(); memcpy(net.players,ps,sizeof(ps)); net.count=count; unlock();
}
static int parse_chat_list(const char *json, ChatMsg *tmp, int cap){
    int tmp_cnt=0;
    const char *p=skip_ws(json);
    if(!p||!*p||!strncmp(p,"null",4)) return 0;
    if(*p!='{') return 0;
    for(p=skip_ws(p+1); p&&*p&&*p!='}'&&tmp_cnt<cap; ){
        if(*p!='\"') break;
        const char *kend=skip_str(p);
        if(!kend) break;
        char key[28]={0};
        size_t kl=(size_t)(kend-p-2);
        if(kl>=sizeof(key)) kl=sizeof(key)-1;
        memcpy(key,p+1,kl); key[kl]=0;
        p=skip_ws(kend);
        if(*p!=':') break;
        p=skip_ws(p+1);
        const char *obj=p;
        p=skip_val(p);
        if(!p) break;
        memset(&tmp[tmp_cnt], 0, sizeof(tmp[tmp_cnt]));
        strncpy(tmp[tmp_cnt].key, key, sizeof(tmp[tmp_cnt].key)-1);
        strv(obj,"uid",tmp[tmp_cnt].uid,sizeof(tmp[tmp_cnt].uid));
        strv(obj,"nick",tmp[tmp_cnt].nick,sizeof(tmp[tmp_cnt].nick));
        strv(obj,"text",tmp[tmp_cnt].text,sizeof(tmp[tmp_cnt].text));
        tmp[tmp_cnt].valid=1;
        tmp_cnt++;
        p=skip_ws(p);
        if(p&&*p==',') p=skip_ws(p+1);
    }
    return tmp_cnt;
}
static void parse_and_store_chat(const char *json){
    ChatMsg tmp[CHAT_MAX+16];
    int tmp_cnt, keep, start, i;
    char extra[CHAT_MAX+16][28];
    int nextra=0;
    if(!json || !*json) return;
    tmp_cnt=parse_chat_list(json, tmp, CHAT_MAX+16);
    if(tmp_cnt<=0) return;
    lock(); keep=chat_keep; unlock();
    if(keep<1) keep=1;
    if(keep>CHAT_MAX) keep=CHAT_MAX;
    start=0;
    if(tmp_cnt>keep){
        int drop=tmp_cnt-keep;
        for(i=0;i<drop;i++){
            if(tmp[i].key[0]){
                strncpy(extra[nextra], tmp[i].key, sizeof(extra[0])-1);
                extra[nextra][sizeof(extra[0])-1]=0;
                nextra++;
            }
        }
        start=drop;
    }
    lock();
    net.chat_count=0;
    for(i=start;i<tmp_cnt && net.chat_count<CHAT_MAX;i++)
        net.chats[net.chat_count++]=tmp[i];
    unlock();
    for(i=0;i<nextra;i++) chat_delete_key_async(extra[i]);
}
static int pull_chat(char *resp, size_t cap){
    char url[URL];
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json?orderBy=%%22$key%%22&limitToLast=%d",net.base,net.room,CHAT_MAX);
    return fb_http("GET",url,NULL,resp,cap)==200;
}
static void prune_old_chat(void){
    char url[URL], resp[CHAT_RESP];
    ChatMsg oldm[16];
    char keepkeys[CHAT_MAX][28];
    int n, nk=0, count, keep, i, j;
    lock(); keep=chat_keep; count=net.chat_count;
    for(i=0;i<net.chat_count;i++){
        strncpy(keepkeys[nk], net.chats[i].key, sizeof(keepkeys[0])-1);
        keepkeys[nk][sizeof(keepkeys[0])-1]=0;
        nk++;
    }
    unlock();
    if(count<keep || keep<1) return;
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json?orderBy=%%22$key%%22&limitToFirst=16",net.base,net.room);
    if(fb_http("GET",url,NULL,resp,sizeof(resp))!=200) return;
    n=parse_chat_list(resp, oldm, 16);
    for(i=0;i<n;i++){
        int kept=0;
        if(!oldm[i].key[0]) continue;
        for(j=0;j<nk;j++) if(!strcmp(oldm[i].key, keepkeys[j])){ kept=1; break; }
        if(!kept) chat_delete_key_async(oldm[i].key);
    }
}

/* Ждём первую загрузку списка банов, но не дольше BAN_WAIT мс. */
static void wait_ban_list(void) {
    long long deadline = now_ms() + BAN_WAIT;
    while (net.run && !ban_synced() && now_ms() < deadline) sleep_ms(50);
}
/* Пауза после неудач: с каждой следующей ошибкой вдвое длиннее (300 мс …
 * 8 с, плюс джиттер). Раньше потоки бились в базу каждые 300–500 мс даже
 * когда она лежала, усиливая аварию. */
static void net_fail_sleep(int fails) {
    int ms = 300;
    for (int i = 1; i < fails && ms < 8000; i++) ms *= 2;
    if (ms > 8000) ms = 8000;
    sleep_ms(ms + (int)(now_ms() % 250));
}
static void mark_self_banned(const char *nick) {
    int fresh;
    lock(); fresh = !net.self_banned; net.self_banned = 1; unlock();
    if (fresh) LOGERR("'%s' is banned: online closed", nick);
}
/* Выкидываем слот забаненного игрока, чтобы он не мешал бою у остальных. */
static void room_slot_delete_async(int slot) {
    char *url = (char*)malloc(FB_URL_MAX);
    DSThread t;
    if (!url || !net.base[0]) { free(url); return; }
    {
        char plain[URL];
        snprintf(plain, sizeof(plain), "%s/rooms/%s/players/%d.json", net.base, net.room, slot);
        fb_url(url, FB_URL_MAX, plain);
    }
    t = ds_thread_start(http_delete_job, url);
    if (t) { ds_thread_detach(t); return; }
    free(url);
}
static void *thread_main(void *arg) {
    int fails=0; (void)arg;
    while(net.run) {
        long long start=now_ms(); int slot;
        lock(); slot=net.slot; unlock();
        if(slot<0) {
            char nick[24];
            status(NET_CONNECTING);
            /* В комнату не пускаем, пока не проверили свой ник по списку банов.
             * Ждём недолго: если сеть лежит, слот всё равно не достанется. */
            wait_ban_list();
            lock(); snprintf(nick,sizeof(nick),"%s",net.me.nick); unlock();
            if(nick[0] && ban_check_cloud_sync(nick)) {
                mark_self_banned(nick);
                sleep_ms(500);
                continue;
            }
            slot=claim_slot();
            if(slot<0){ if(++fails>6){status(NET_ERROR); LOGERR("network error: cannot claim player slot");} net_fail_sleep(fails); continue; }
            lock(); net.slot=slot; net.seq=0; net.players[slot]=net.me; net.players[slot].online=1; unlock(); fails=0;
            LOG("slot %d claimed", (int)net.slot);
        }
        if(!push_state()){ if(++fails>6){status(NET_ERROR); LOGERR("network error: failed to push state");} net_fail_sleep(fails); continue; }
        fails=0; status(NET_PLAYING);
        long long spent=now_ms()-start; if(spent<WRITE_TICK)sleep_ms((int)(WRITE_TICK-spent));
    }
    release_slot(); status(NET_OFFLINE); return NULL;
}
static void *reader_thread(void *arg) {
    char resp[RESP], chat_resp[CHAT_RESP], event_resp[RESP], banner_resp[512];
    long long next_chat=0, next_event=0, next_ban=0, next_prune=0, next_banner=0;
    int fails=0;
    (void)arg;
    while(net.run) {
        long long start=now_ms(); int slot;
        /* Список банов тянем и пока подключаемся, и во время боя: забаненный
         * игрок не должен ни зайти, ни остаться в комнате. */
        if(start>=next_ban) {
            if(ban_sync()) {
                char nick[24];
                lock(); snprintf(nick,sizeof(nick),"%s",net.me.nick); unlock();
                if(nick[0] && ban_cache_has(nick)) {
                    mark_self_banned(nick);
                    net.run=0;
                    break;
                }
            }
            next_ban=now_ms()+BAN_TICK;
        }
        lock(); slot=net.slot; unlock();
        if(slot<0){ sleep_ms(50); continue; }
        if(pull_state(resp,sizeof(resp))) { read_players(resp); fails=0; }
        else { if(fails<20) fails++; net_fail_sleep(fails); }
        if(start>=next_chat) {
            if(pull_chat(chat_resp,sizeof(chat_resp))) {
                parse_and_store_chat(chat_resp);
                /* Чистку старых сообщений гоняем редко: это отдельный GET,
                 * который базе не нужен каждую секунду. */
                if(start>=next_prune) { prune_old_chat(); next_prune=now_ms()+CHAT_PRUNE_TICK; }
            }
            next_chat=now_ms()+CHAT_TICK;
        }
        if(start>=next_event) {
            if(pull_event(event_resp,sizeof(event_resp))) {
                int ev=(int)num(event_resp,"",0);
                if(ev<0) ev=0;
                lock(); net.event=ev; unlock();
            }
            next_event=now_ms()+EVENT_TICK;
        }
        if(start>=next_banner) {
            if(pull_banner(banner_resp,sizeof(banner_resp)))
                parse_and_store_banner(banner_resp);
            next_banner=now_ms()+BANNER_TICK;
        }
        long long spent=now_ms()-start; if(spent<READ_TICK)sleep_ms((int)(READ_TICK-spent));
    }
    return NULL;
}
static void make_uid(void) {
    unsigned long a,b; int local=0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    a=(unsigned long)time(NULL)^((unsigned long)t.tv_nsec<<8);
    b=(unsigned long)getpid()^(unsigned long)(uintptr_t)&local;
    snprintf(net.uid,sizeof(net.uid),"%08lx%08lx",a&0xfffffffful,b&0xfffffffful);
}
void net_connect(const char *url, const char *room) {
    size_t n; if(!url||!*url)return;
    net_fast = 0;
    if(net.started)net_disconnect();
    memset(&net.me,0,sizeof(net.me));
    snprintf(net.base,sizeof(net.base),"%s",url); n=strlen(net.base); while(n&&net.base[n-1]=='/')net.base[--n]=0;
    snprintf(net.room,sizeof(net.room),"%s",(room&&*room)?room:"main");
    if(!net.uid[0])make_uid();
    {
        char snick[LOGIN_NICK_MAX+1];
        lg_lock(); if (lg.status == NET_LOGIN_OK) snprintf(snick,sizeof(snick),"%s",lg.session_nick); else snick[0]=0; lg_unlock();
        if (snick[0]) snprintf(net.me.nick,sizeof(net.me.nick),"%s",snick);
    }
    net.me.cls = (double)pending_cls;
    net.me.level = (double)pending_level;
    net.me.skin = (double)pending_skin;
    lock(); net.self_banned = 0; unlock();
    /* Список банов нужно перечитать: вдруг бан поставили, пока мы сидели в меню. */
    ban_invalidate();
    /* Сбрасываем всё состояние прошлой комнаты: чат, игроков, ивент/баннер и
     * статики наблюдения за слотами. Без этого, пока новый клиент ещё не
     * захватил слот (комната занята «призраками» убитых приложений), на
     * экране висят сообщения и снимки игроков из прошлой сессии — выглядит
     * как «чат прогрузился, а никто не двигается, висит подключение». */
    memset(claim_seen_seq, 0, sizeof(claim_seen_seq));
    memset(claim_seen_at, 0, sizeof(claim_seen_at));
    memset(claim_seen_uid, 0, sizeof(claim_seen_uid));
    memset(rp_lseq, 0, sizeof(rp_lseq));
    memset(rp_lch, 0, sizeof(rp_lch));
    memset(rp_kicked, 0, sizeof(rp_kicked));
    memset(&ps_last, 0, sizeof(ps_last));
    ps_last_put = 0; ps_have_last = 0;
    lock();
    memset(net.players, 0, sizeof(net.players));
    memset(net.chats, 0, sizeof(net.chats));
    net.chat_count = 0;
    net.count = 0;
    net.slot = -1;
    net.seq = 0;
    net.event = 0;
    net.banner_text[0] = 0;
    net.banner_color[0] = 0;
    net.banner_ts = 0;
    unlock();
    net.started=1;
    net.status=NET_CONNECTING; net.run=1;
    LOG("connect %s/%s", net.base, net.room);
    net.thread = ds_thread_start(thread_main, NULL);
    net.rthread = ds_thread_start(reader_thread, NULL);
}
void net_disconnect(void) {
    if(!net.started)return;
    net_fast = 1;
    net.run=0;
    ds_thread_join(net.thread); net.thread=(DSThread)0;
    ds_thread_join(net.rthread); net.rthread=(DSThread)0;
    net.started=0; net.slot=-1; status(NET_OFFLINE);
    net_fast = 0;
}
void net_publish(double x, double y, double angle, double hp, double alive) {
    lock();
    net.me.x=x; net.me.y=y; net.me.a=angle; net.me.hp=hp; net.me.alive=alive;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
