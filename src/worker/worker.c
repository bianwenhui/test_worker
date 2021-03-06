
#include "worker.h"
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>

#include "tpool.h"

#define EPOLL_TMO 30

typedef struct {
        int fd;
        char name[MAX_NAME_LEN];
        int multi;
        worker_exec_t exec;
        worker_queue_t queue;
        void *ctx;
} event_t;

typedef struct {
        tpool_t tpool;
        int epoll_fd;
        event_t *array[0];
} worker_t;

static worker_t *worker;

uint64_t nofile_cur;
uint64_t nofile_max;

int nofile_init()
{
        int ret;
        struct rlimit rlim, rlim_new;

        if (getrlimit(RLIMIT_CORE, &rlim) == 0) {
                rlim_new.rlim_cur = rlim_new.rlim_max = RLIM_INFINITY;
                if (setrlimit(RLIMIT_CORE, &rlim_new) != 0) {
                        rlim_new.rlim_cur = rlim_new.rlim_max 
                                = rlim.rlim_max;
                        (void) setrlimit(RLIMIT_CORE, &rlim_new);
                }
        }

        ret = getrlimit(RLIMIT_NOFILE, &rlim_new);
        if (ret == -1) {
                ret = errno;
                DWARN("%d - %s\n", ret, strerror(ret));
        }

        nofile_cur = rlim_new.rlim_cur;
        nofile_max = rlim_new.rlim_max;
        return 0;
}



static int __worker_add(int fd)
{
        int ret;
        struct epoll_event ev;

        ev.data.fd = fd;
        ev.events = EPOLLIN | EPOLLET;
        ret = _epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
        if (ret == -1) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

inline static int __worker_del(int fd)
{
        int ret;
        struct epoll_event ev;

        ev.data.fd = fd;
        ev.events = EPOLLIN | EPOLLET;
        ret = _epoll_ctl(worker->epoll_fd, EPOLL_CTL_DEL, fd, &ev);
        if (ret == -1) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}


static void *__worker(void *_args)
{
        int ret, nfds, retry;
        struct epoll_event ev;
        event_t *event;
        uint64_t notify;
        time_t now;

        (void) _args;

        while (1) {
                DWARN("tpool wait 0\n");
                ret = tpool_wait(&worker->tpool);
                if (ret)
                        GOTO(err_ret, ret);

                DWARN("tpool wait 1\n");
                now = gettime();
                if (worker->tpool.last_left) {
                        if (now - worker->tpool.last_left > 2) {
                                DWARN("wait %u\n", (int)(now - worker->tpool.last_left));
                        }
                }

                DBUG("worker in, total %u, last %u\n", worker->tpool.total,
                      (int)worker->tpool.last_left);

                while (1) {
                        DWARN("epoll wait 0\n");
                        nfds = _epoll_wait(worker->epoll_fd, &ev, 1, EPOLL_TMO * 1000);
                        if (nfds < 0) {
                                ret = -nfds;
                                GOTO(err_ret, ret);
                        }
                        DWARN("epoll wait 1\n");

                        if (nfds == 0)
                                continue;

                        YASSERT(nfds == 1);

                        event = worker->array[ev.data.fd];

                        if (event->multi == 0) {
                                ret = __worker_del(event->fd);
                                if (ret)
                                        GOTO(err_ret, ret);
                        }

                        ret = read(event->fd, &notify, sizeof(notify));
                        if (ret < 0) {
                                ret = errno;
                                GOTO(err_ret, ret);
                        }

                        retry = 0;
                retry:
                        ret = tpool_left(&worker->tpool);
                        if (ret) {
                                DWARN("all thread busy, retry %u\n", retry);
                                sleep(1);
                                retry++;
                                goto retry;
                        }

                        DBUG("worker left, total %u, event %s\n", worker->tpool.total, event->name);

                        event->exec(event->ctx);

                        if (event->multi == 0) {
                                ret = __worker_add(event->fd);
                                if (ret)
                                        GOTO(err_ret, ret);
                        }

                        tpool_return(&worker->tpool);
                        break;
                }
        }

        return NULL;
err_ret:
        YASSERT(0);
        return NULL;
}


int worker_init()
{
        int ret, size;

        nofile_init();
        DINFO("----nofile_max %llu\n", nofile_max);

        size = sizeof(worker_t) + sizeof(event_t *) * nofile_max;
        ret = ymalloc((void **)&worker, size);
        if (ret)
                GOTO(err_ret, ret);

        memset(worker, 0x0, size);

        worker->epoll_fd = epoll_create(nofile_max);
        if (worker->epoll_fd == -1) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        ret = tpool_init(&worker->tpool, __worker, worker, "worker", 128);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}


int worker_create(worker_handler_t *handler, const char *name,
                  worker_exec_t exec, worker_queue_t queue, void *ctx, int type, int multi)
{
        int ret, fd;
        event_t *event;

        if (type == WORKER_TYPE_TIMER) {
                fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
                if (fd < 0) {
                        ret = errno;
                        GOTO(err_ret, ret);
                }
        } else if (type ==WORKER_TYPE_SEM) {
                fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
                if (fd < 0) {
                        ret = errno;
                        GOTO(err_ret, ret);
                }
        } else {
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        YASSERT((uint64_t)fd < nofile_max);
        YASSERT(worker->array[fd] == NULL);

        ret = ymalloc((void **)&event, sizeof(*event));
        if (ret)
                GOTO(err_ret, ret);

        event->fd = fd;
        event->ctx = ctx;
        event->queue = queue;
        event->multi = multi;
        event->exec = exec;
        strcpy(event->name, name);
        worker->array[fd] = event;

        tpool_increase(&worker->tpool);

        ret = __worker_add(fd);
        if (ret)
                GOTO(err_free, ret);

        handler->fd = fd;
        handler->type = type;

        DINFO("create worker %s\n", name);

        return 0;
err_free:
        yfree((void **)&event);
err_ret:
        return ret;
}

int worker_queue(const worker_handler_t *handler, const void *arg)
{
        int ret;
        event_t *event;

        if (handler->type != WORKER_TYPE_SEM) {
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        event = worker->array[handler->fd];
        ret = event->queue(event->ctx, arg);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}



int worker_post(const worker_handler_t *handler)
{
        int ret;
        uint64_t notify;

        if (handler->type != WORKER_TYPE_SEM) {
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        notify = 1;
        ret = write(handler->fd, &notify, sizeof(notify));
        if (ret < 0) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int worker_settime(const worker_handler_t *handler, uint64_t nsec)
{
        int ret;
        struct itimerspec new_value;

        if (handler->type != WORKER_TYPE_TIMER) {
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        new_value.it_interval.tv_nsec = 0;
        new_value.it_interval.tv_sec = 0;
        new_value.it_value.tv_sec = nsec / 1000000;
        new_value.it_value.tv_nsec = nsec % 1000000;

        ret = timerfd_settime(handler->fd, 0, &new_value, NULL);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

