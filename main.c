#include <android_native_app_glue.h>
#include "runtime.h"

struct engine { struct android_app* app; };

static void handle_cmd(struct android_app* app, int32_t cmd) {
    struct engine* e = (struct engine*)app->userData;
    if (cmd == APP_CMD_INIT_WINDOW) {
        screen_w = ANativeWindow_getWidth(app->window);
        screen_h = ANativeWindow_getHeight(app->window);
        ANativeWindow_setBuffersGeometry(app->window, 0, 0, WINDOW_FORMAT_RGBA_8888);
        init(app->activity->assetManager);
    }
}

static int32_t handle_input(struct android_app* app, AInputEvent* event) {
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        float x = AMotionEvent_getX(event, 0);
        float y = AMotionEvent_getY(event, 0);
        int action = AMotionEvent_getAction(event);
        touch(x, y, action);
        return 1;
    }
    return 0;
}

void android_main(struct android_app* app) {
    struct engine e = {0};
    app->userData = &e;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;
    
    while (1) {
        struct android_poll_source* source;
        while (ALooper_pollOnce(0, 0, 0, (void**)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) return;
        }
        
        if (app->window) {
            update();
            ANativeWindow_Buffer buf;
            if (ANativeWindow_lock(app->window, &buf, 0) == 0) {
                Buffer rb = { (uint32_t*)buf.bits, buf.width, buf.height, buf.stride };
                draw(&rb);
                ANativeWindow_unlockAndPost(app->window);
            }
        }
    }
}
