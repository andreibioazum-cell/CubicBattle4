/* Room threads: ban list wait, failure backoff, the writer and reader network
 * threads and the public net_connect / net_disconnect / net_publish entry points.
 * Split out of room_sync.inc; included by net.c after room_chat.inc. */
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