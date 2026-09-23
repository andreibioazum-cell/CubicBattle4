/* The last state sent successfully, used as anti-flood by push_state. net_connect
 * resets it so the first send of a new session is not compared with the previous
 * one. */
static Actor ps_last;
static long long ps_last_put = 0;
static int ps_have_last = 0;

/* Whether the work of a network thread still belongs to the live connection. A
 * thread checks this before applying an answer: the answer of a request that was
 * already in flight when the player left the room must not land in the state of
 * the next room, which is how a stale chat and stale players appeared there. */
static int net_gen_ok(unsigned gen) { return net.run && net.gen == gen; }
static int push_state(void) {
    Actor a; int slot; unsigned long seq; char url[URL], body[BODY], enick[64];
    lock(); a=net.me; slot=net.slot; seq=++net.seq; unlock();
    if(slot<0) return 0;
    /* Standing still with a fresh heartbeat means the database is left alone, but seq
     * has to update at least once per HEARTBEAT_TICK, otherwise other players hit
     * TIMEOUT, take the slot for dead and claim it. */
    if (ps_have_last && actor_same(&a,&ps_last) && now_ms()-ps_last_put < HEARTBEAT_TICK) return 1;
    json_escape(a.nick,enick,sizeof(enick));
    snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot);
    snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"x\":%.5f,\"y\":%.5f,\"angle\":%.5f,\"hp\":%.0f,\"alive\":%.0f,\"seq\":%lu,\"px\":%.5f,\"py\":%.5f,\"pdx\":%.5f,\"pdy\":%.5f,\"punch\":%.0f,\"sx\":%.5f,\"sy\":%.5f,\"sdx\":%.5f,\"sdy\":%.5f,\"snow\":%.0f,\"stx\":%.5f,\"sty\":%.5f,\"sthp\":%.0f,\"s2x\":%.5f,\"s2y\":%.5f,\"s2hp\":%.0f,\"s3x\":%.5f,\"s3y\":%.5f,\"s3hp\":%.0f,\"station\":%.0f,\"dax\":%.5f,\"day\":%.5f,\"dadx\":%.5f,\"dady\":%.5f,\"dash\":%.0f,\"uzx\":%.5f,\"uzy\":%.5f,\"universe\":%.0f,\"thud\":%.0f,\"cls\":%.0f,\"level\":%.0f,\"skin\":%.0f}",
        net.uid,enick,clamp01(a.x),clamp01(a.y),safe(a.a),safe(a.hp),safe(a.alive),seq,
        clamp01(a.punch_x),clamp01(a.punch_y),clamp11(a.punch_dx),clamp11(a.punch_dy),safe(a.punch),
        clamp01(a.snow_x),clamp01(a.snow_y),clamp11(a.snow_dx),clamp11(a.snow_dy),safe(a.snow),
        clamp01(a.station_x),clamp01(a.station_y),safe(a.station_hp),
        clamp01(a.station2_x),clamp01(a.station2_y),safe(a.station2_hp),
        clamp01(a.station3_x),clamp01(a.station3_y),safe(a.station3_hp),
        safe(a.station),
        clamp01(a.dash_x),clamp01(a.dash_y),clamp11(a.dash_dx),clamp11(a.dash_dy),safe(a.dash),
        clamp01(a.universe_x),clamp01(a.universe_y),safe(a.universe),safe(a.thud),safe(a.cls),safe(a.level),safe(a.skin));
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
static void parse_and_store_banner(const char *json, unsigned gen) {
    char text[CHAT_TEXT_MAX], color[BANNER_COLOR_MAX];
    long long ts;
    if (!json || !*json || !strncmp(json,"null",4)) return;
    ts = (long long)num(json,"ts",0);
    if (ts <= 0) return;
    strv(json,"text",text,sizeof(text));
    strv(json,"color",color,sizeof(color));
    if (!text[0]) return;
    lock();
    if (!net_gen_ok(gen)) { unlock(); return; }
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

/* Removes our row from the room. The slot is passed in rather than read from the
 * net state, because net_disconnect clears it at once while the network thread
 * still has to give up the row it claimed. */
static void release_slot(int slot) {
    char url[URL];
    if(slot<0)return;
    snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot); fb_http("DELETE",url,NULL,NULL,0);
}
static int claim_slot(unsigned gen) {
    char resp[RESP],url[URL],body[BODY],uid[24],etag[96]; long long t=now_ms(); int slot;
    for(slot=0;slot<NET_SLOTS;slot++) {
        unsigned long seq; int claim=0,code;
        if (!net_gen_ok(gen)) return -1;
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
            snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"x\":0,\"y\":0,\"angle\":0,\"hp\":0,\"alive\":0,\"seq\":0,\"px\":0,\"py\":0,\"pdx\":0,\"pdy\":0,\"punch\":0,\"sx\":0,\"sy\":0,\"sdx\":0,\"sdy\":0,\"snow\":0,\"stx\":0,\"sty\":0,\"sthp\":0,\"s2x\":0,\"s2y\":0,\"s2hp\":0,\"s3x\":0,\"s3y\":0,\"s3hp\":0,\"station\":0,\"dax\":0,\"day\":0,\"dadx\":0,\"dady\":0,\"dash\":0,\"uzx\":0,\"uzy\":0,\"universe\":0,\"thud\":0,\"cls\":%.0f,\"level\":%.0f,\"skin\":%.0f}",net.uid,enick,safe(net.me.cls),safe(net.me.level),safe(net.me.skin));
        }
        code=fb_http_ex("PUT",url,body,NULL,0, etag[0] ? "if-match" : NULL, etag[0] ? etag : NULL, NULL, 0);
        if(code==200) { claim_seen_uid[slot][0]=0; claim_seen_seq[slot]=0; claim_seen_at[slot]=0; LOG("slot %d uid %s",slot,net.uid); return slot; }
        if(net_log_ok()) LOGERR("claim slot %d: PUT failed, HTTP %d", slot, code);
    }
    return -1;
}
static void read_players(const char *resp, unsigned gen) {
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
            /* A banned player is neither shown nor kept in the room, which is how a ban also
             * works on those who joined with an older build. */
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
        snprintf(p,sizeof(p),"%s/s2x",bp); ps[slot].station2_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/s2y",bp); ps[slot].station2_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/s2hp",bp); ps[slot].station2_hp=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/s3x",bp); ps[slot].station3_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/s3y",bp); ps[slot].station3_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/s3hp",bp); ps[slot].station3_hp=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/station",bp); ps[slot].station=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/dax",bp); ps[slot].dash_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/day",bp); ps[slot].dash_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/dadx",bp); ps[slot].dash_dx=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/dady",bp); ps[slot].dash_dy=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/dash",bp); ps[slot].dash=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/uzx",bp); ps[slot].universe_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/uzy",bp); ps[slot].universe_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/universe",bp); ps[slot].universe=num(resp,p,0);
        /* Turret counter hit: old clients send no such field, so it reads as 0, no events
         * are generated, and mixed versions keep working online. */
        snprintf(p,sizeof(p),"%s/thud",bp); ps[slot].thud=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/cls",bp); ps[slot].cls=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/level",bp); ps[slot].level=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/skin",bp); ps[slot].skin=num(resp,p,0);
        ps[slot].online=1; count++;
    }
    lock();
    if (net_gen_ok(gen)) { memcpy(net.players,ps,sizeof(ps)); net.count=count; }
    unlock();
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
static void parse_and_store_chat(const char *json, unsigned gen){
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
    if (!net_gen_ok(gen)) { unlock(); return; }
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
static void prune_old_chat(unsigned gen){
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
    if(!net_gen_ok(gen)) return;
    n=parse_chat_list(resp, oldm, 16);
    for(i=0;i<n;i++){
        int kept=0;
        if(!oldm[i].key[0]) continue;
        for(j=0;j<nk;j++) if(!strcmp(oldm[i].key, keepkeys[j])){ kept=1; break; }
        if(!kept) chat_delete_key_async(oldm[i].key);
    }
}

/* Waits for the first ban list load, but no longer than BAN_WAIT ms. */
static void wait_ban_list(void) {
    long long deadline = now_ms() + BAN_WAIT;
    while (net.run && !ban_synced() && now_ms() < deadline) sleep_ms(50);
}
/* Backoff after failures: every next error doubles the pause, from 300 ms up to
 * 8 s plus jitter. The threads used to hammer the database every 300 to 500 ms even
 * while it was down, making the outage worse. */
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
/* Drops the slot of a banned player so he stops disturbing the fight for the rest. */
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
    unsigned gen=(unsigned)(uintptr_t)arg;
    int fails=0, mine=-1;
    while(net_gen_ok(gen)) {
        long long start=now_ms(); int slot;
        lock(); slot=net.slot; unlock();
        if(slot<0) {
            char nick[24];
            status(NET_CONNECTING);
            /* A player stays out of the room until his own nick was checked against
             * the ban list, and the wait is short: a dead network would not yield
             * the slot anyway. */
            wait_ban_list();
            lock(); snprintf(nick,sizeof(nick),"%s",net.me.nick); unlock();
            if(nick[0] && ban_check_cloud_sync(nick)) {
                mark_self_banned(nick);
                sleep_ms(500);
                continue;
            }
            slot=claim_slot(gen);
            if(slot<0){ if(++fails>6){status(NET_ERROR); LOGERR("network error: cannot claim player slot");} net_fail_sleep(fails); continue; }
            lock(); net.slot=slot; net.seq=0; net.players[slot]=net.me; net.players[slot].online=1; unlock(); fails=0; mine=slot;
            LOG("slot %d claimed", (int)net.slot);
        }
        if(!push_state()){ if(++fails>6){status(NET_ERROR); LOGERR("network error: failed to push state");} net_fail_sleep(fails); continue; }
        fails=0; status(NET_PLAYING);
        long long spent=now_ms()-start; if(spent<WRITE_TICK)sleep_ms((int)(WRITE_TICK-spent));
    }
    /* The row goes away so nobody sees a ghost, but only while this connection is
     * still the current one: after a reconnect the slot number belongs to the new
     * session, which is already publishing into it. */
    if (net.gen == gen) { release_slot(mine); status(NET_OFFLINE); }
    return NULL;
}
static void *reader_thread(void *arg) {
    char resp[RESP], chat_resp[CHAT_RESP], event_resp[RESP], banner_resp[512];
    long long next_chat=0, next_event=0, next_ban=0, next_prune=0, next_banner=0;
    unsigned gen=(unsigned)(uintptr_t)arg;
    int fails=0;
    while(net_gen_ok(gen)) {
        long long start=now_ms(); int slot;
        /* The ban list is fetched while connecting and during the battle: a banned player
         * must neither enter nor stay in the room. */
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
        if(pull_state(resp,sizeof(resp)) && net_gen_ok(gen)) { read_players(resp,gen); fails=0; }
        else { if(fails<20) fails++; net_fail_sleep(fails); }
        if(start>=next_chat) {
            if(pull_chat(chat_resp,sizeof(chat_resp)) && net_gen_ok(gen)) {
                parse_and_store_chat(chat_resp,gen);
                /* Old chat messages are cleaned up rarely, since it is a separate GET the database
                 * does not need every second. */
                if(start>=next_prune) { prune_old_chat(gen); next_prune=now_ms()+CHAT_PRUNE_TICK; }
            }
            next_chat=now_ms()+CHAT_TICK;
        }
        if(start>=next_event) {
            if(pull_event(event_resp,sizeof(event_resp)) && net_gen_ok(gen)) {
                int ev=(int)num(event_resp,"",0);
                if(ev<0) ev=0;
                lock(); net.event=ev; unlock();
            }
            next_event=now_ms()+EVENT_TICK;
        }
        if(start>=next_banner) {
            if(pull_banner(banner_resp,sizeof(banner_resp)) && net_gen_ok(gen))
                parse_and_store_banner(banner_resp,gen);
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
    size_t n; unsigned gen; if(!url||!*url)return;
    net_fast = 0;
    if(net.started)net_disconnect();
    /* A new generation: threads of the previous connection stop and their late
     * answers are dropped instead of landing in the state of this room. */
    lock(); gen=++net.gen; unlock();
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
    /* The ban list has to be read again, in case a ban arrived while the menu was open. */
    ban_invalidate();
    /* Everything from the previous room is reset: the chat, the players, the event or
     * banner and the slot watching statics. Without it, while the new client has not
     * claimed the slot yet and the room is held by ghosts of killed apps, the screen
     * shows messages and player snapshots from the last session, which looks like a
     * chat that loaded while nobody moves and the connection hangs. */
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
    net.thread = ds_thread_start(thread_main, (void *)(uintptr_t)gen);
    net.rthread = ds_thread_start(reader_thread, (void *)(uintptr_t)gen);
}
void net_disconnect(void) {
    DSThread th, rth;
    if(!net.started)return;
    net_fast = 1;
    lock();
    net.run=0;
    net.slot=-1;
    th=net.thread; rth=net.rthread;
    net.thread=(DSThread)0; net.rthread=(DSThread)0;
    unlock();
    /* The threads are detached and left to exit on their own. Joining them here
     * held the game thread until both finished the HTTP request they were inside,
     * so leaving the battle froze the frames and the input, and the system ended
     * up showing an application-not-responding dialog. A thread now stops at its
     * next generation check and drops the answer it was waiting for, and the fast
     * mode shortens the request that is still in flight. */
    ds_thread_detach(th); ds_thread_detach(rth);
    net.started=0;
    status(NET_OFFLINE);
    net_fast = 0;
}
void net_publish(double x, double y, double angle, double hp, double alive) {
    lock();
    net.me.x=x; net.me.y=y; net.me.a=angle; net.me.hp=hp; net.me.alive=alive;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();