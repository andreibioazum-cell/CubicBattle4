#!/usr/bin/env bash
# Build the headless Linux test harness. Temporarily points the game at the
# local fake Firebase server, generates game.c, builds, then restores the
# real Firebase URL from config.ds and regenerates game.c.
set -e
cd "$(dirname "$0")/.."

REAL_URL="https://cubicbattleserver-19ae2-default-rtdb.firebaseio.com"
TEST_URL="http://127.0.0.1:18765"

cp game/config.ds /tmp/config.ds.bak
trap 'cp /tmp/config.ds.bak game/config.ds; python3 gen.py >/dev/null' EXIT

sed -i "s|$REAL_URL|$TEST_URL|" game/config.ds
python3 gen.py >/dev/null

# Regenerate the net.c copy with the test HTTP hook.
python3 - <<'EOF'
src = open('net.c').read()
old = '''#else
static int http_ex(const char *method,const char *url,const char *body,char *out,size_t cap,
                   const char *header,const char *value,char *etag,size_t etag_cap) {
    (void)method; (void)url; (void)body; (void)header; (void)value;
    if (out && cap) out[0] = '\\0';
    if (etag && etag_cap) etag[0] = '\\0';
    return 0;
}
#endif'''
new = '''#else
extern int test_http_impl(const char *method,const char *url,const char *body,char *out,size_t cap,
                   const char *header,const char *value,char *etag,size_t etag_cap);
static int http_ex(const char *method,const char *url,const char *body,char *out,size_t cap,
                   const char *header,const char *value,char *etag,size_t etag_cap) {
    return test_http_impl(method,url,body,out,cap,header,value,etag,etag_cap);
}
#endif'''
assert old in src, "desktop http_ex block not found"
open('test/net_test.c','w').write(src.replace(old,new))
EOF

gcc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -D_POSIX_C_SOURCE=200809L -I. -Ig \
    test/main_linux.c test/net_test.c runtime.c graphics.c game/game.c \
    -lpthread -lm -o test/game_test
echo "test/game_test built"
