/* Login by nick and password: net_auth as a whole.
 *
 * The body of the function used to be split between auth_session.inc and
 * moderation.inc, one function across two files, which left no room to include
 * anything between them. Both halves now live here, and the include order in
 * net.c stays as it was: auth_session.inc, net_auth_flow.inc,
 * moderation.inc.
 *
 * Parsing the profile (apply_user_json and apply_user_json_keep_local) and
 * pushing the local progress into the cloud (push_pg_to_cloud) live in
 * profile_apply.inc.
 */
double net_auth(const char *url, const char *nick, const char *pass) {
    if (!nick || !nick_valid(nick)) return (double)NET_LOGIN_BAD_NICK;
    if (!pass || !pass_valid(pass)) return (double)NET_LOGIN_BAD_PASS;
    const char *base = (url && *url) ? url : net.base;
    if (!base || !*base) return (double)NET_ERROR;
    if (!net.base[0] && url && *url) {
        snprintf(net.base, sizeof(net.base), "%s", url);
        size_t n = strlen(net.base);
        while (n && net.base[n - 1] == '/') net.base[--n] = 0;
    }

    char req_url[URL], resp[RESP];
    snprintf(req_url, sizeof(req_url), "%s/users/%s.json", net.base, nick);
    if (fb_enabled()) {
        int registered = 0;
        int r;
        if (!nick_ascii_ok(nick)) {
            /* The account e-mail is built from the nick, so the protected mode
             * allows Latin letters, digits and "_" only. */
            LOGERR("'%s': in secured mode nick must be latin letters, digits or '_'", nick);
            return (double)NET_LOGIN_BAD_NICK;
        }
        r = fb_sign_in(nick, pass, &registered);
        if (r == 1) return (double)NET_LOGIN_WRONG_PASS;
        if (r != 0) return (double)NET_ERROR;
        int code = fb_http("GET", req_url, NULL, resp, sizeof(resp));
        const char *p = skip_ws(resp);
        if (code == 0) {
            if (net_log_ok()) LOGERR("net_auth: network error connecting to %s", req_url);
            return (double)NET_ERROR;
        }
        if (code != 200 || !p || !*p || !strncmp(p, "null", 4)) {
            /* No record yet, either a new account or an old one without a record,
             * so a default record without a password is created. */
            int put_code = put_default_user(req_url, nick, NULL);
            if (put_code != 200) {
                if (net_log_ok()) LOGERR("net_auth: create user record failed HTTP %d", put_code);
                return (double)NET_ERROR;
            }
            net_auth_session_ok(nick, pass);
            LOG("%s and logged in: '%s'", registered ? "registered" : "account restored", nick);
            return (double)NET_LOGIN_OK;
        }
        if (*p == '{') {
            int restored = apply_user_json_keep_local(resp);
            lg_lock();
            snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
            snprintf(lg.session_pass, sizeof(lg.session_pass), "%s", pass);
            lg.status = NET_LOGIN_OK;
            lg_unlock();
            session_save(nick, pass);
            if (restored) push_pg_to_cloud();
            LOG("logged in: '%s' (cups=%d, candies=%d)", nick,
                (int)num(resp, "cups", 0), (int)num(resp, "candies", 0));
            return (double)NET_LOGIN_OK;
        }
        return (double)NET_ERROR;
    }

    int code = http("GET", req_url, NULL, resp, sizeof(resp));
    if (code == 0) {
        if (net_log_ok()) LOGERR("net_auth: network error connecting to %s", req_url);
        return (double)NET_ERROR;
    }

    const char *p = skip_ws(resp);
    if (!p || !*p || !strncmp(p, "null", 4) || code == 404) {
        int put_code = put_default_user(req_url, nick, pass);
        if (put_code != 200) {
            if (net_log_ok()) LOGERR("net_auth: register failed HTTP %d", put_code);
            return (double)NET_ERROR;
        }
        net_auth_session_ok(nick, pass);
        LOG("registered and logged in: '%s'", nick);
        return (double)NET_LOGIN_OK;
    }

    if (*p == '{') {
        char got_pass[64] = "";
        strv(resp, "pass", got_pass, sizeof(got_pass));
        if (strcmp(got_pass, pass) == 0) {
            int restored = apply_user_json_keep_local(resp);
            lg_lock();
            snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
            snprintf(lg.session_pass, sizeof(lg.session_pass), "%s", pass);
            lg.status = NET_LOGIN_OK;
            lg_unlock();
            session_save(nick, pass);
            if (restored) push_pg_to_cloud();
            LOG("logged in: '%s' (cups=%d, candies=%d)", nick,
                (int)num(resp, "cups", 0), (int)num(resp, "candies", 0));
            return (double)NET_LOGIN_OK;
        } else {
            if (net_log_ok()) LOG("wrong password for '%s'", nick);
            return (double)NET_LOGIN_WRONG_PASS;
        }
    }

    return (double)NET_ERROR;
}