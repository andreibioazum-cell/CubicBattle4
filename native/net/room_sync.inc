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
