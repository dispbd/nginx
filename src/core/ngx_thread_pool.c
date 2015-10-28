
/*
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Valentin V. Bartenev
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_semaphore.h>
#include <ngx_thread_pool.h>


typedef struct {
    ngx_array_t               pools;
} ngx_thread_pool_conf_t;


typedef struct {
    ngx_atomic_t              first;
    ngx_atomic_t              last_p;
} ngx_thread_pool_queue_t;


struct ngx_thread_pool_s {
    ngx_sem_t                 sem;

    ngx_uint_t                task_id;

    ngx_thread_pool_queue_t   in;
    ngx_thread_pool_queue_t   out;

    ngx_connection_t         *notify;

    ngx_log_t                *log;
    ngx_pool_t               *pool;

    ngx_str_t                 name;
    ngx_uint_t                threads;
    ngx_uint_t                max_queue;

    u_char                   *file;
    ngx_uint_t                line;
};


static ngx_int_t ngx_thread_pool_init(ngx_thread_pool_t *tp, ngx_log_t *log,
    ngx_pool_t *pool);
static void ngx_thread_pool_destroy(ngx_thread_pool_t *tp);

static void *ngx_thread_pool_cycle(void *data);
static void ngx_thread_pool_handler(ngx_event_t *ev);

static char *ngx_thread_pool(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static void *ngx_thread_pool_create_conf(ngx_cycle_t *cycle);
static char *ngx_thread_pool_init_conf(ngx_cycle_t *cycle, void *conf);

static ngx_int_t ngx_thread_pool_init_worker(ngx_cycle_t *cycle);
static void ngx_thread_pool_exit_worker(ngx_cycle_t *cycle);


static ngx_command_t  ngx_thread_pool_commands[] = {

    { ngx_string("thread_pool"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE23,
      ngx_thread_pool,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_core_module_t  ngx_thread_pool_module_ctx = {
    ngx_string("thread_pool"),
    ngx_thread_pool_create_conf,
    ngx_thread_pool_init_conf
};


ngx_module_t  ngx_thread_pool_module = {
    NGX_MODULE_V1,
    &ngx_thread_pool_module_ctx,           /* module context */
    ngx_thread_pool_commands,              /* module directives */
    NGX_CORE_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_thread_pool_init_worker,           /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_thread_pool_exit_worker,           /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_str_t  ngx_thread_pool_default = ngx_string("default");

static ngx_atomic_t  ngx_thread_counter = 1;


static ngx_int_t
ngx_thread_pool_init(ngx_thread_pool_t *tp, ngx_log_t *log, ngx_pool_t *pool)
{
    int             err;
    pthread_t       tid;
    ngx_uint_t      n;
    pthread_attr_t  attr;

    if (ngx_signal_notify == NULL) {
        ngx_log_error(NGX_LOG_ALERT, log, 0,
               "the configured event method cannot be used with thread pools");
        return NGX_ERROR;
    }

    if (ngx_sem_init(&tp->sem, 0, log) != NGX_OK) {
        return NGX_ERROR;
    }

    tp->in.first = (ngx_atomic_uint_t) NULL;
    tp->in.last_p = (ngx_atomic_uint_t) &tp->in.first;

    tp->out.first = (ngx_atomic_uint_t) NULL;
    tp->out.last_p = (ngx_atomic_uint_t) &tp->out.first;

    tp->log = log;
    tp->pool = pool;

    tp->notify = ngx_create_notify(ngx_thread_pool_handler, tp, log);

    if (tp->notify == NULL) {
        return NGX_ERROR;
    }

    err = pthread_attr_init(&attr);
    if (err) {
        ngx_log_error(NGX_LOG_ALERT, log, err,
                      "pthread_attr_init() failed");
        return NGX_ERROR;
    }

#if 0
    err = pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN);
    if (err) {
        ngx_log_error(NGX_LOG_ALERT, log, err,
                      "pthread_attr_setstacksize() failed");
        return NGX_ERROR;
    }
#endif

    for (n = 0; n < tp->threads; n++) {
        err = pthread_create(&tid, &attr, ngx_thread_pool_cycle, tp);
        if (err) {
            ngx_log_error(NGX_LOG_ALERT, log, err,
                          "pthread_create() failed");
            return NGX_ERROR;
        }
    }

    (void) pthread_attr_destroy(&attr);

    return NGX_OK;
}


static void
ngx_thread_pool_destroy(ngx_thread_pool_t *tp)
{
    /* TODO: exit threads */

    (void) ngx_sem_destroy(&tp->sem, tp->log);

    ngx_destroy_notify(tp->notify);
}


ngx_thread_pool_task_t *
ngx_thread_pool_task_alloc(ngx_pool_t *pool, size_t size)
{
    ngx_thread_pool_task_t  *task;

    task = ngx_pcalloc(pool, sizeof(ngx_thread_pool_task_t) + size);
    if (task == NULL) {
        return NULL;
    }

    task->ctx = task + 1;

    return task;
}


ngx_int_t
ngx_thread_pool_task_post(ngx_thread_pool_t *tp, ngx_thread_pool_task_t *task)
{
    ngx_int_t          count;
    ngx_atomic_uint_t  lp;

    if (task->event.active) {
        ngx_log_error(NGX_LOG_ALERT, tp->log, 0,
                      "task #%ui already active", task->id);
        return NGX_ERROR;
    }

    if (ngx_sem_getvalue(&tp->sem, &count, tp->log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (count >= (ngx_int_t) tp->max_queue) {
        ngx_log_error(NGX_LOG_ERR, tp->log, 0,
                      "thread pool \"%V\" queue overflow: %i tasks waiting",
                      &tp->name, count);
        return NGX_ERROR;
    }

    task->event.active = 1;

    task->id = tp->task_id++;
    task->next = NULL;

    lp = tp->in.last_p;

    if (lp == (ngx_atomic_uint_t) &tp->in.first
        || !ngx_atomic_cmp_set(&tp->in.last_p, lp,
                               (ngx_atomic_uint_t) &task->next))
    {
        /*
         * either this is the first task or the last one
         * has just been dequeued by a thread
         */

        tp->in.first = (ngx_atomic_uint_t) task;
        tp->in.last_p = (ngx_atomic_uint_t) &task->next;

    } else {
        *(ngx_thread_pool_task_t **) lp = task;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_CORE, tp->log, 0,
                   "task #%ui added to thread pool \"%V\"",
                   task->id, &tp->name);

    if (ngx_sem_post(&tp->sem, tp->log) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void *
ngx_thread_pool_cycle(void *data)
{
    ngx_thread_pool_t *tp = data;

    int                      err;
    sigset_t                 set;
    ngx_log_t                log;
    ngx_connection_t         notify;
    ngx_atomic_uint_t        lp;
    ngx_thread_pool_task_t  *task;

    log = *tp->log;
    log.thread = ngx_atomic_fetch_add(&ngx_thread_counter, 1);
    log.time = 1;
    log.update_time = 1;

    ngx_log_debug2(NGX_LOG_DEBUG_CORE, &log, 0,
                   "thread %uL of thread pool \"%V\" started",
                   (uint64_t) ngx_thread_tid(), &tp->name);

    sigfillset(&set);

    err = pthread_sigmask(SIG_BLOCK, &set, NULL);
    if (err) {
        ngx_log_error(NGX_LOG_ALERT, &log, err, "pthread_sigmask() failed");
        return NULL;
    }

    notify = *tp->notify;
    notify.log = &log;

    for ( ;; ) {
        if (ngx_sem_wait(&tp->sem, &log) != NGX_OK) {
            return NULL;
        }

        log.update_time = 1;

again:

        do {
            task = (ngx_thread_pool_task_t *) tp->in.first;

            if (task == NULL) {
                ngx_thread_yield();
                goto again;
            }

        } while (!ngx_atomic_cmp_set(&tp->in.first, (ngx_atomic_uint_t) task,
                                     (ngx_atomic_uint_t) task->next));

        if (tp->in.first == (ngx_atomic_uint_t) NULL) {
            /* special care to avoid race condition with appending */

            if (tp->in.last_p != (ngx_atomic_uint_t) &task->next
                || !ngx_atomic_cmp_set(&tp->in.last_p,
                                       (ngx_atomic_uint_t) &task->next,
                                       (ngx_atomic_uint_t) &tp->in.first))
            {
                if (*(volatile void **) &task->next == NULL) {
                    tp->in.first = (ngx_atomic_uint_t) task;
                    ngx_thread_yield();
                    goto again;
                }

                tp->in.first = (ngx_atomic_uint_t) task->next;
            }
        }

        ngx_log_debug2(NGX_LOG_DEBUG_CORE, &log, 0,
                       "run task #%ui in thread pool \"%V\"",
                       task->id, &tp->name);

        task->handler(task->ctx, &log);

        ngx_log_debug2(NGX_LOG_DEBUG_CORE, &log, 0,
                       "complete task #%ui in thread pool \"%V\"",
                       task->id, &tp->name);

        task->next = NULL;

        ngx_memory_barrier();

        do {
            lp = tp->out.last_p;
        } while (!ngx_atomic_cmp_set(&tp->out.last_p, lp,
                                     (ngx_atomic_uint_t) &task->next));

        *(ngx_thread_pool_task_t **) lp = task;

        (void) ngx_signal_notify(&notify);
    }

    return NULL;
}


static void
ngx_thread_pool_handler(ngx_event_t *ev)
{
    ngx_event_t             *event;
    ngx_thread_pool_t       *tp;
    ngx_thread_pool_task_t  *task;

    tp = ev->data;

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, ev->log, 0,
                   "thread pool \"%V\" handler", &tp->name);

    if (ngx_handle_notify) {
        (void) ngx_handle_notify(tp->notify);
    }

    for ( ;; ) {
        task = (ngx_thread_pool_task_t *) tp->out.first;

        if (task == NULL) {
            return;
        }

        tp->out.first = (ngx_atomic_uint_t) task->next;

        if (tp->out.first == (ngx_atomic_uint_t) NULL) {
            /* special care to avoid race condition with appending */

            if (tp->out.last_p != (ngx_atomic_uint_t) &task->next
                || !ngx_atomic_cmp_set(&tp->out.last_p,
                                       (ngx_atomic_uint_t) &task->next,
                                       (ngx_atomic_uint_t) &tp->out.first))
            {
                tp->out.first = (ngx_atomic_uint_t) task;
                return;
            }
        }

        ngx_log_debug2(NGX_LOG_DEBUG_CORE, tp->log, 0,
                       "run completion handler for task #%ui "
                       "in thread pool \"%V\"", task->id, &tp->name);

        event = &task->event;

        event->complete = 1;
        event->active = 0;

        event->handler(event);
    }
}


static void *
ngx_thread_pool_create_conf(ngx_cycle_t *cycle)
{
    ngx_thread_pool_conf_t  *tcf;

    tcf = ngx_pcalloc(cycle->pool, sizeof(ngx_thread_pool_conf_t));
    if (tcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&tcf->pools, cycle->pool, 4,
                       sizeof(ngx_thread_pool_t *))
        != NGX_OK)
    {
        return NULL;
    }

    return tcf;
}


static char *
ngx_thread_pool_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_thread_pool_conf_t *tcf = conf;

    ngx_uint_t           i;
    ngx_thread_pool_t  **tpp;

    tpp = tcf->pools.elts;

    for (i = 0; i < tcf->pools.nelts; i++) {

        if (tpp[i]->threads) {
            continue;
        }

        if (tpp[i]->name.len == ngx_thread_pool_default.len
            && ngx_strncmp(tpp[i]->name.data, ngx_thread_pool_default.data,
                           ngx_thread_pool_default.len)
               == 0)
        {
            tpp[i]->threads = 32;
            tpp[i]->max_queue = 65536;
            continue;
        }

        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "unknown thread pool \"%V\" in %s:%ui",
                      &tpp[i]->name, tpp[i]->file, tpp[i]->line);

        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_thread_pool(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t          *value;
    ngx_uint_t          i;
    ngx_thread_pool_t  *tp;

    value = cf->args->elts;

    tp = ngx_thread_pool_add(cf, &value[1]);

    if (tp == NULL) {
        return NGX_CONF_ERROR;
    }

    if (tp->threads) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "duplicate thread pool \"%V\"", &tp->name);
        return NGX_CONF_ERROR;
    }

    tp->max_queue = 65536;

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "threads=", 8) == 0) {

            tp->threads = ngx_atoi(value[i].data + 8, value[i].len - 8);

            if (tp->threads == (ngx_uint_t) NGX_ERROR || tp->threads == 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid threads value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "max_queue=", 10) == 0) {

            tp->max_queue = ngx_atoi(value[i].data + 10, value[i].len - 10);

            if (tp->max_queue == (ngx_uint_t) NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid max_queue value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }
    }

    if (tp->threads == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"threads\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


ngx_thread_pool_t *
ngx_thread_pool_add(ngx_conf_t *cf, ngx_str_t *name)
{
    ngx_thread_pool_t       *tp, **tpp;
    ngx_thread_pool_conf_t  *tcf;

    if (name == NULL) {
        name = &ngx_thread_pool_default;
    }

    tp = ngx_thread_pool_get(cf->cycle, name);

    if (tp) {
        return tp;
    }

    tp = ngx_pcalloc(cf->pool, sizeof(ngx_thread_pool_t));
    if (tp == NULL) {
        return NULL;
    }

    tp->name = *name;
    tp->file = cf->conf_file->file.name.data;
    tp->line = cf->conf_file->line;

    tcf = (ngx_thread_pool_conf_t *) ngx_get_conf(cf->cycle->conf_ctx,
                                                  ngx_thread_pool_module);

    tpp = ngx_array_push(&tcf->pools);
    if (tpp == NULL) {
        return NULL;
    }

    *tpp = tp;

    return tp;
}


ngx_thread_pool_t *
ngx_thread_pool_get(ngx_cycle_t *cycle, ngx_str_t *name)
{
    ngx_uint_t                i;
    ngx_thread_pool_t       **tpp;
    ngx_thread_pool_conf_t   *tcf;

    tcf = (ngx_thread_pool_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                  ngx_thread_pool_module);

    tpp = tcf->pools.elts;

    for (i = 0; i < tcf->pools.nelts; i++) {

        if (tpp[i]->name.len == name->len
            && ngx_strncmp(tpp[i]->name.data, name->data, name->len) == 0)
        {
            return tpp[i];
        }
    }

    return NULL;
}


static ngx_int_t
ngx_thread_pool_init_worker(ngx_cycle_t *cycle)
{
    ngx_uint_t                i;
    ngx_thread_pool_t       **tpp;
    ngx_thread_pool_conf_t   *tcf;

    if (ngx_process != NGX_PROCESS_WORKER
        && ngx_process != NGX_PROCESS_SINGLE)
    {
        return NGX_OK;
    }

    tcf = (ngx_thread_pool_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                  ngx_thread_pool_module);

    if (tcf == NULL) {
        return NGX_OK;
    }

    tpp = tcf->pools.elts;

    for (i = 0; i < tcf->pools.nelts; i++) {
        if (ngx_thread_pool_init(tpp[i], cycle->log, cycle->pool) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static void
ngx_thread_pool_exit_worker(ngx_cycle_t *cycle)
{
    ngx_uint_t                i;
    ngx_thread_pool_t       **tpp;
    ngx_thread_pool_conf_t   *tcf;

    if (ngx_process != NGX_PROCESS_WORKER
        && ngx_process != NGX_PROCESS_SINGLE)
    {
        return;
    }

    tcf = (ngx_thread_pool_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                  ngx_thread_pool_module);

    if (tcf == NULL) {
        return;
    }

    tpp = tcf->pools.elts;

    for (i = 0; i < tcf->pools.nelts; i++) {
        ngx_thread_pool_destroy(tpp[i]);
    }
}
