#include <errno.h>
#include <stddef.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "Python.h"
#include "uv.h"


#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

#ifdef __APPLE__
#define PLATFORM_IS_APPLE 1
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#else
#define PLATFORM_IS_APPLE 0
#endif


#ifdef __linux__
#  define PLATFORM_IS_LINUX 1
#  include <sys/epoll.h>
#else
#  define PLATFORM_IS_LINUX 0
#  define EPOLL_CTL_DEL 2
struct epoll_event {};
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    return 0;
};
#endif


PyObject *
MakeUnixSockPyAddr(struct sockaddr_un *addr)
{
    if (addr->sun_family != AF_UNIX) {
        PyErr_SetString(
            PyExc_ValueError, "a UNIX socket addr was expected");
        return NULL;
    }

#ifdef __linux__
    int addrlen = sizeof (struct sockaddr_un);
    size_t linuxaddrlen = addrlen - offsetof(struct sockaddr_un, sun_path);
    if (linuxaddrlen > 0 && addr->sun_path[0] == 0) {
        return PyBytes_FromStringAndSize(addr->sun_path, linuxaddrlen);
    }
    else
#endif /* linux */
    {
        /* regular NULL-terminated string */
        return PyUnicode_DecodeFSDefault(addr->sun_path);
    }
}


#if PY_VERSION_HEX < 0x03070100

PyObject * Context_CopyCurrent(void) {
    return (PyObject *)PyContext_CopyCurrent();
};

int Context_Enter(PyObject *ctx) {
    return PyContext_Enter((PyContext *)ctx);
}

int Context_Exit(PyObject *ctx) {
    return PyContext_Exit((PyContext *)ctx);
}

#else

PyObject * Context_CopyCurrent(void) {
    return PyContext_CopyCurrent();
};

int Context_Enter(PyObject *ctx) {
    return PyContext_Enter(ctx);
}

int Context_Exit(PyObject *ctx) {
    return PyContext_Exit(ctx);
}

#endif

/* inlined from cpython/Modules/signalmodule.c
 * https://github.com/python/cpython/blob/v3.13.0a6/Modules/signalmodule.c#L1931-L1951
 * private _Py_RestoreSignals has been moved to CPython internals in Python 3.13
 * https://github.com/python/cpython/pull/106400 */

void
_Py_RestoreSignals(void)
{
#ifdef SIGPIPE
    PyOS_setsig(SIGPIPE, SIG_DFL);
#endif
#ifdef SIGXFZ
    PyOS_setsig(SIGXFZ, SIG_DFL);
#endif
#ifdef SIGXFSZ
    PyOS_setsig(SIGXFSZ, SIG_DFL);
#endif
}


#if __APPLE__

typedef struct {
    int errorno;
    uv_loop_t* loop;
    CFRunLoopTimerRef timer;
    CFRunLoopSourceRef rls;
    int status;
    int depth;
    uv_timer_t* watcher;
    uv_check_t* trigger;
} CFUVInfo;

void timer_cb(uv_timer_t *handle) {
    CFUVInfo* cfuv_info = (CFUVInfo*) handle->data;
    uv_timer_stop(handle);  // 타이머가 반복 스케줄링에 영향을 주지 않도록 즉시 중지
    if (handle->loop->stop_flag != 0) {
        cfuv_info->status |= 0b10;
        
        uv_close(cfuv_info->watcher, NULL);
        uv_close(cfuv_info->trigger, NULL);
        CFRunLoopStop(CFRunLoopGetCurrent());
        cfuv_info->trigger = NULL;
        cfuv_info->watcher = NULL;
    }
}

void check_cb(uv_check_t *handle) {
    CFUVInfo* cfuv_info = (CFUVInfo*) handle->data;
    uv_timer_start(cfuv_info->watcher, timer_cb, 0, 0);  // 타이머를 짧은 시간 후 호출되도록 설정
}

static inline void CF_BACKEND_CALLBACK(CFUVInfo* cfuv_info) {
    if (cfuv_info->loop->stop_flag != 0) {
        cfuv_info->status |= 0b10;
        CFRunLoopStop(CFRunLoopGetCurrent());
            if (cfuv_info->trigger) {
                uv_check_stop(cfuv_info->trigger);
                uv_close(cfuv_info->trigger, NULL);
            }
            if (cfuv_info->watcher) {
                uv_timer_stop(cfuv_info->watcher);
                uv_close(cfuv_info->watcher, NULL);
            }
        cfuv_info->trigger = NULL;
        cfuv_info->watcher = NULL;
    }
    cfuv_info->status |= 0b01;
    int status = uv_run(cfuv_info->loop, UV_RUN_NOWAIT);
    if (status == 0) {
        cfuv_info->status |= 0b10;
        if (cfuv_info->rls) {
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), cfuv_info->rls, kCFRunLoopCommonModes);
        }
        if (cfuv_info->timer) {
            CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), cfuv_info->timer, kCFRunLoopCommonModes);
        }
    }
    cfuv_info->errorno = status;

}

static void UV_CF_OBSERVER_CALLBACK(CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info) {
    CFUVInfo* cfuv_info = (CFUVInfo*) info;

    switch (activity) {
        case kCFRunLoopEntry:
        cfuv_info->depth++;
        break;
        case kCFRunLoopExit:
        cfuv_info->depth--;
        break;
        case kCFRunLoopBeforeWaiting:
        if (cfuv_info->loop->stop_flag != 0) {
            cfuv_info->status |= 0b10;
        }
        if (uv_loop_alive(cfuv_info->loop) > 0) {
            uv_update_time(cfuv_info->loop);
            int timeout = uv_backend_timeout(cfuv_info->loop);
            if (timeout == -1) {
                if (cfuv_info->loop->stop_flag == 0) {
                    CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), cfuv_info->timer, kCFRunLoopCommonModes);
                }
                // printf("cf_uv_run block forever\n");
                uv_print_active_handles(cfuv_info->loop,NULL);
            } else {
                CFRunLoopTimerSetNextFireDate(cfuv_info->timer, CFAbsoluteTimeGetCurrent() + ( CFAbsoluteTime) timeout * 0.001);
                CFRunLoopAddTimer(CFRunLoopGetCurrent(), cfuv_info->timer, kCFRunLoopCommonModes);
            }
        }
        if (cfuv_info->status & 0b10) {
            if (cfuv_info->trigger) {
                uv_check_stop(cfuv_info->trigger);
                uv_close(cfuv_info->trigger, NULL);
            }
            if (cfuv_info->watcher) {
                uv_timer_stop(cfuv_info->watcher);
                uv_close(cfuv_info->watcher, NULL);
            }
            cfuv_info->trigger = NULL;
            cfuv_info->watcher = NULL;
        }
        if (cfuv_info->depth < 2 && (cfuv_info->status & 0b10)) {
            CFRunLoopSourceSignal(cfuv_info->rls);
            CFRunLoopStop(CFRunLoopGetCurrent());
        }
        break;
    }
}

static void UV_SOURCE_CALLBACK(CFFileDescriptorRef f, CFOptionFlags callBackTypes, void *info) {
    CFUVInfo* cfuv_info = (CFUVInfo*) info;
    // printf("UV_SOURCE_CALLBACK \n");
    CF_BACKEND_CALLBACK(cfuv_info);
}


static void UV_TIMER_CALLBACK(CFRunLoopTimerRef f, void *info) {
    CFUVInfo* cfuv_info = (CFUVInfo*) info;
    // printf("UV_TIMER_CALLBACK \n");
    if (cfuv_info->rls) {
        CFRunLoopSourceSignal(cfuv_info->rls);
        CFRunLoopWakeUp(CFRunLoopGetCurrent());
    }
    CF_BACKEND_CALLBACK(cfuv_info);
}


int cf_uv_run(uv_loop_t* loop, uv_run_mode mode) {
    id (*msgSend_id)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
    void (*msgSend_void)(id, SEL) = (void (*)(id, SEL))objc_msgSend;
    CFUVInfo info = {0, loop, NULL, NULL, 0, 0, NULL, NULL};

    CFFileDescriptorContext context = {0, &info, NULL, NULL, NULL};
    CFFileDescriptorRef fd = CFFileDescriptorCreate(
        0, uv_backend_fd(loop), false, UV_SOURCE_CALLBACK, &context
    );
    CFRunLoopTimerContext timer_context = {0, &info, NULL, NULL, NULL};
    CFFileDescriptorEnableCallBacks(fd, kCFFileDescriptorReadCallBack);
    info.rls = CFFileDescriptorCreateRunLoopSource(NULL, fd, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), info.rls, kCFRunLoopCommonModes);
    info.timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent(), 24 * 3600, 0, 0, UV_TIMER_CALLBACK, &timer_context);
    CFRunLoopObserverContext observer_context = {0, &info, NULL, NULL, NULL};
    CFRunLoopObserverRef observer = CFRunLoopObserverCreate(NULL, kCFRunLoopAllActivities, true, 0, UV_CF_OBSERVER_CALLBACK, &observer_context);
    CFRunLoopAddObserver(CFRunLoopGetCurrent(), observer, kCFRunLoopCommonModes);
    CFRunLoopTimerSetTolerance(info.timer, 0.001);
    id pool;
    int timeout = 0;
    CFRunLoopRunResult result;
    CFAbsoluteTime duration = 0;
    info.errorno = uv_loop_alive(loop);
    uv_check_t check_ref;
    uv_timer_t timer_ref;
    info.trigger = &check_ref;
    info.watcher = &timer_ref;    
    uv_timer_init(loop, &timer_ref);
    uv_check_init(loop, &check_ref);
    check_ref.data = &info;
    timer_ref.data = &info;

    uv_check_start(info.trigger, check_cb);
    uv_unref(&timer_ref);
    uv_unref(&check_ref);
    do {     
       if (info.status & 0b10) {
            break;
        }
        info.status &= 0b10;

        info.errorno = 0;
        timeout = uv_backend_timeout(loop);
        if (mode == UV_RUN_NOWAIT) {
            duration = 0;
        } else {
            duration = 24 * 3600;
        }
        pool = msgSend_id((id)objc_getClass("NSAutoreleasePool"), sel_getUid("alloc"));
        pool = msgSend_id(pool, sel_getUid("init"));
        result = CFRunLoopRunInMode(kCFRunLoopDefaultMode, duration, true);
        msgSend_void(pool, sel_getUid("drain"));

        
        if (mode == UV_RUN_NOWAIT || mode == UV_RUN_ONCE) {
            break;
        }
        if ( result == kCFRunLoopRunFinished || result == kCFRunLoopRunStopped) {
            break;
        }
        if (info.status & 0b01) {
            // uv_run called
            
        } else {

            // uv_run not called
        }
        
    } while (uv_loop_alive(loop) > 0);


    CFRunLoopObserverInvalidate(observer);
    if (info.timer) {
        CFRunLoopTimerInvalidate(info.timer);
        CFRelease(info.timer);
        info.timer = NULL;
    }
    if (info.rls) {
        CFRunLoopSourceInvalidate(info.rls);
        CFRelease(info.rls);
        info.rls = NULL;
    }
    if (fd) {
        CFFileDescriptorInvalidate(fd);
        CFRelease(fd);
        fd = NULL;
    }
    if (info.trigger) {
        uv_check_stop(info.trigger);
        uv_close(info.trigger, NULL);
        info.trigger = NULL;
    }
    if (info.watcher) {
        uv_timer_stop(info.watcher);
        uv_close(info.watcher, NULL);
        info.watcher = NULL;
    }
    
    uv_run(loop, UV_RUN_NOWAIT);
    uv_run(loop, UV_RUN_NOWAIT);
    return info.errorno;
}

#else


int cf_uv_run(uv_loop_t* loop, uv_run_mode mode) {
    return uv_run(loop, mode);
}


#endif