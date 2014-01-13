#include <fbuf.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <errno.h>
#include <pthread.h>
#include <iomux.h>
#include <queue.h>

#include "messaging.h"
#include "connections.h"
#include "shardcache.h"
#include "counters.h"
#include "rbuf.h"

#include "serving.h"

#ifndef HAVE_UINT64_T
#define HAVE_UINT64_T
#endif
#include <siphash.h>

#define ATOMIC_READ(__v) __sync_fetch_and_add(&(__v), 0)

#define ATOMIC_INCREMENT(__v) __sync_add_and_fetch(&(__v), 1)

#define ATOMIC_DECREMENT(__v) __sync_sub_and_fetch(&(__v), 1)

#define ATOMIC_CAS(__v, __o, __n) __sync_bool_compare_and_swap(&(__v), __o, __n)

#define ATOMIC_SET(__v, __n) {\
    int __b = 0;\
    do {\
        __b = __sync_bool_compare_and_swap(&__v, ATOMIC_READ(__v), __n);\
    } while (!__b);\
}

typedef struct {
    pthread_t thread;
    queue_t *jobs;
    uint32_t busy;
    uint32_t num_fds;
    int leave;
    pthread_cond_t wakeup_cond;
    pthread_mutex_t wakeup_lock;
} shardcache_worker_context_t;

struct __shardcache_serving_s {
    shardcache_t *cache;
    int sock;
    pthread_t listener;
    const char *auth;
    const char *me;
    int num_workers;
    shardcache_worker_context_t *workers;
    shardcache_counters_t *counters;
};

typedef struct __shardcache_connection_context_s shardcache_connection_context_t;

typedef void (*shardcache_get_remainder_callback_t)(shardcache_connection_context_t *ctx);

struct __shardcache_connection_context_s {
    shardcache_hdr_t hdr;
    shardcache_hdr_t sig_hdr;
    fbuf_t *output;
    int fd;
    shardcache_t *cache;
#define SHARDCACHE_CONNECTION_CONTEX_RECORDS_MAX 4
    fbuf_t records[SHARDCACHE_CONNECTION_CONTEX_RECORDS_MAX];
    size_t remainder;
    shardcache_get_remainder_callback_t get_remainder_cb;
    const char    *auth;
    shardcache_worker_context_t *worker_ctx;
    async_read_ctx_t *reader_ctx;
};

static void
async_read_handler(void *data, size_t len, int idx, void *priv)
{
    shardcache_connection_context_t *ctx =
        (shardcache_connection_context_t *)priv;

    switch(idx) {
        case 0:
            fbuf_add_binary(&ctx->records[0], data, len);
            break;
        case 1:
            fbuf_add_binary(&ctx->records[1], data, len);
            break;
        case 2:
            if (len == 4) {
            } else {
                // TODO - Error Messages
            }
            break;
        default:
            // TODO - Error Messages
            break;
    }
}


static shardcache_connection_context_t *
shardcache_connection_context_create(shardcache_t *cache,
                                     const char *auth,
                                     int fd)
{
    shardcache_connection_context_t *context =
        calloc(1, sizeof(shardcache_connection_context_t));

    context->output = fbuf_create(0);

    context->cache = cache;
    context->auth = auth;
    context->fd = fd;
    context->reader_ctx = async_read_context_create((char *)auth,
                                                    async_read_handler,
                                                    context);
    return context;
}

static void
shardcache_connection_context_destroy(shardcache_connection_context_t *ctx)
{
    fbuf_free(ctx->output);
    int i;
    for (i = 0; i < SHARDCACHE_CONNECTION_CONTEX_RECORDS_MAX; i++)
        fbuf_destroy(&ctx->records[i]);
    async_read_context_destroy(ctx->reader_ctx);
    free(ctx);
}

static void
write_status(shardcache_connection_context_t *ctx, int rc)
{
    char out[20];

    out[0] = 0;

    char *p = &out[2];
    if (rc != 0) {
        out[1] = 3;
        strncpy(p, "ERR", 3);
        p += 3;
    } else {
        out[1] = 2;
        strncpy(p, "OK", 2);
        p += 2;
    }
    memset(p, 0, 3);
    p += 3;

    sip_hash *shash = NULL;
    if (ctx->auth) {
        unsigned char hdr_sig = ctx->sig_hdr;
        fbuf_add_binary(ctx->output, (char *)&hdr_sig, 1);
        shash = sip_hash_new((char *)ctx->auth, 2, 4);
    }

    uint16_t initial_offset = fbuf_used(ctx->output);

    unsigned char hdr = SHARDCACHE_HDR_RES;
    fbuf_add_binary(ctx->output, (char *)&hdr, 1);
    if (ctx->auth && ctx->sig_hdr == SHARDCACHE_HDR_CSIG_SIP) {
        uint64_t digest;
        sip_hash_digest_integer(shash, &hdr, 1, &digest);
        fbuf_add_binary(ctx->output, (char *)&digest, sizeof(digest));
    }

    fbuf_add_binary(ctx->output, out, p - &out[0]);

    if (ctx->auth) {
        uint64_t digest;
        if (ctx->sig_hdr == SHARDCACHE_HDR_CSIG_SIP) {
            sip_hash_digest_integer(shash, out, p - &out[0], &digest);
        } else {
            sip_hash_digest_integer(shash,
                                    fbuf_data(ctx->output) + initial_offset,
                                    p - &out[0] + 1, &digest);
        }
        fbuf_add_binary(ctx->output, (char *)&digest, sizeof(digest));
    }
    if (shash)
        sip_hash_free(shash);
}

static void *
process_request(void *priv)
{
    shardcache_connection_context_t *ctx =
        (shardcache_connection_context_t *)priv;
    shardcache_t *cache = ctx->cache;

    int rc = 0;
    void *key = fbuf_data(&ctx->records[0]);
    size_t klen = fbuf_used(&ctx->records[0]);

    switch(ctx->hdr) {
        case SHARDCACHE_HDR_OFX:
        {
            fbuf_t out = FBUF_STATIC_INITIALIZER;
            char buf[1<<16];
            size_t vlen = sizeof(buf); 

            uint32_t offset = 0;
            if (fbuf_used(&ctx->records[2]) == 4) {
                memcpy(&offset, fbuf_data(&ctx->records[2]), sizeof(uint32_t));
                offset = ntohl(offset);
            } else {
                // TODO - Error Messages
            }

            ctx->remainder = shardcache_get_offset(cache, key, klen, buf, &vlen, offset, NULL);
            if (vlen) {
                uint32_t remainder = htonl(ctx->remainder);
                if (build_message((char *)ctx->auth,
                                  ctx->sig_hdr,
                                  SHARDCACHE_HDR_RES,
                                  buf, vlen,
                                  (void *)&remainder, sizeof(remainder),
                                  0, &out) == 0)
                {
                    fbuf_add_binary(ctx->output, fbuf_data(&out), fbuf_used(&out));
                }
                else
                {
                    // TODO - Error Messages
                }
            }
            fbuf_destroy(&out);
        }
        case SHARDCACHE_HDR_GET:
        {
            //if (shardcache_test_ownership(cache, key, klen, NULL, 0)) {
                fbuf_t out = FBUF_STATIC_INITIALIZER;
                size_t vlen = 0;
                void *v = shardcache_get(cache, key, klen, &vlen, NULL);
                if (build_message((char *)ctx->auth,
                                  ctx->sig_hdr,
                                  SHARDCACHE_HDR_RES,
                                  v, vlen,
                                  NULL, 0,
                                  0, &out) == 0)
                {
                    fbuf_add_binary(ctx->output, fbuf_data(&out), fbuf_used(&out));
                }
                else
                {
                    // TODO - Error Messages
                }
                fbuf_destroy(&out);

                if (v)
                    free(v);
            /*} else {
                fbuf_t out = FBUF_STATIC_INITIALIZER;
                char buf[1<<16];
                size_t vlen = sizeof(buf); 

                ctx->remainder = shardcache_get_offset(cache, key, klen, buf, &vlen, 0, NULL);
                if (vlen) {
                    uint32_t remainder = htonl(ctx->remainder);
                    if (build_message((char *)ctx->auth,
                                      ctx->sig_hdr,
                                      SHARDCACHE_HDR_RES,
                                      buf, vlen,
                                      (void *)&remainder, sizeof(remainder),
                                      0, &out) == 0)
                    {
                        fbuf_add_binary(ctx->output, fbuf_data(&out), fbuf_used(&out));
                    }
                    else
                    {
                        // TODO - Error Messages
                    }
                }
                fbuf_destroy(&out);

            }*/
            break;
        }
        case SHARDCACHE_HDR_SET:
        {

            if (fbuf_used(&ctx->records[2]) == 4) {
                uint32_t expire;
                memcpy(&expire, fbuf_data(&ctx->records[2]), sizeof(uint32_t));
                expire = ntohl(expire);
                rc = shardcache_set_volatile(cache,
                                             key,
                                             klen,
                                             fbuf_data(&ctx->records[1]),
                                             fbuf_used(&ctx->records[1]),
                                             expire);

            } else {
                rc = shardcache_set(cache, key, klen,
                        fbuf_data(&ctx->records[1]), fbuf_used(&ctx->records[1]));
            }
            write_status(ctx, rc);
            break;
        }
        case SHARDCACHE_HDR_DEL:
        {
            rc = shardcache_del(cache, key, klen);
            write_status(ctx, rc);
            break;
        }
        case SHARDCACHE_HDR_EVI:
        {
            shardcache_evict(cache, key, klen);
            write_status(ctx, 0);
            break;
        }
        case SHARDCACHE_HDR_MGB:
        {
            int num_shards = 0;
            shardcache_node_t *nodes = NULL;
            char *s = (char *)fbuf_data(&ctx->records[0]);
            while (s && *s) {
                char *tok = strsep(&s, ",");
                if(tok) {
                    char *label = strsep(&tok, ":");
                    char *addr = tok;
                    num_shards++;
                    size_t size = num_shards * sizeof(shardcache_node_t);
                    nodes = realloc(nodes, size);
                    shardcache_node_t *node = &nodes[num_shards-1];
                    snprintf(node->label, sizeof(node->label), "%s", label);
                    snprintf(node->address, sizeof(node->address), "%s", addr);
                } 
            }
            rc = shardcache_migration_begin(cache, nodes, num_shards, 0);
            if (rc != 0) {
                // TODO - Error messages
            }
            free(nodes);
            write_status(ctx, 0);
            break;
        }
        case SHARDCACHE_HDR_MGA:
        {
            rc = shardcache_migration_abort(cache);
            write_status(ctx, rc);
            break;
        }
        case SHARDCACHE_HDR_MGE:
        {
            rc = shardcache_migration_end(cache);
            write_status(ctx, rc);
            break;
        }
        case SHARDCACHE_HDR_CHK:
        {
            // TODO - HEALTH CHECK
            write_status(ctx, 0);
            break;
        }
        case SHARDCACHE_HDR_STS:
        {
            fbuf_t buf = FBUF_STATIC_INITIALIZER;
            shardcache_counter_t *counters = NULL;
            int i, num_nodes;

            shardcache_node_t *nodes = shardcache_get_nodes(cache, &num_nodes);
            if (nodes) {
                fbuf_printf(&buf, "num_nodes;%d\r\nnodes;", num_nodes);
                for (i = 0; i < num_nodes; i++) {
                    if (i > 0)
                        fbuf_add(&buf, ",");
                    fbuf_printf(&buf, "%s:%s",
                                nodes[i].label, nodes[i].address);
                }
                fbuf_add(&buf, "\r\n");
                free(nodes);
            }

            int ncounters = shardcache_get_counters(cache, &counters);
            if (counters) {
                for (i = 0; i < ncounters; i++) {
                    fbuf_printf(&buf, "%s;%u\r\n",
                                counters[i].name, counters[i].value);
                }

                fbuf_t out = FBUF_STATIC_INITIALIZER;
                if (build_message((char *)ctx->auth,
                                  ctx->sig_hdr,
                                  SHARDCACHE_HDR_RES,
                                  fbuf_data(&buf), fbuf_used(&buf),
                                  NULL, 0,
                                  0, &out) == 0)
                {
                    fbuf_add_binary(ctx->output,
                            fbuf_data(&out), fbuf_used(&out));
                } else {
                    // TODO - Error Messages
                }
                fbuf_destroy(&out);
                free(counters);
            }
            fbuf_destroy(&buf);
            break;
        }
        case SHARDCACHE_HDR_IDG:
        {
            fbuf_t buf = FBUF_STATIC_INITIALIZER;
            fbuf_t out = FBUF_STATIC_INITIALIZER;
            shardcache_storage_index_t *index = shardcache_get_index(cache);
            if (index) {
                int i;
                for (i = 0; i < index->size; i++) {
                    uint32_t klen = (uint32_t)index->items[i].klen;
                    uint32_t vlen = (uint32_t)index->items[i].vlen;
                    void *key = index->items[i].key;
                    uint32_t nklen = htonl(klen);
                    uint32_t nvlen = htonl(vlen);
                    fbuf_add_binary(&buf, (char *)&nklen, sizeof(nklen));
                    fbuf_add_binary(&buf, key, klen);
                    fbuf_add_binary(&buf, (char *)&nvlen, sizeof(nvlen));
                }
                size_t zero = 0;
                // no klen terminates the list
                fbuf_add_binary(&buf, (char *)&zero, sizeof(zero));

                shardcache_free_index(index); 
            }

            // chunkize the data and build an actual message
            if (build_message((char *)ctx->auth,
                              ctx->sig_hdr,
                              SHARDCACHE_HDR_IDR,
                              fbuf_data(&buf), fbuf_used(&buf),
                              NULL, 0,
                              0, &out) == 0)
            {
                // destroy it early ... since we still need one more copy
                fbuf_destroy(&buf);
                fbuf_add_binary(ctx->output, fbuf_data(&out), fbuf_used(&out));
            } else {
                fbuf_destroy(&buf);
                // TODO - Error Messages
            }
            fbuf_destroy(&out);
            break;
        }
        default:
            fprintf(stderr, "Unsupported command: 0x%02x\n", (char)ctx->hdr);
            break;
    }

    return NULL;
}

static void
shardcache_output_handler(iomux_t *iomux, int fd, void *priv)
{
    shardcache_connection_context_t *ctx =
        (shardcache_connection_context_t *)priv;

    if (fbuf_used(ctx->output)) {
        int wb = iomux_write(iomux, fd,
                            fbuf_data(ctx->output),
                            fbuf_used(ctx->output));
        fbuf_remove(ctx->output, wb);
    }

    if (!fbuf_used(ctx->output)) {
        iomux_callbacks_t *cbs = iomux_callbacks(iomux, fd);
        cbs->mux_output = NULL;
    }
}

static void
shardcache_input_handler(iomux_t *iomux,
                         int fd,
                         void *data,
                         int len,
                         void *priv)
{
    shardcache_connection_context_t *ctx =
        (shardcache_connection_context_t *)priv;

    if (!ctx)
        return;

    // new data arrived so we want to run the asyncrhonous reader to update
    // the context
    async_read_context_input_data(data, len, ctx->reader_ctx);

    // if a complete message has been handled (so we are back to
    // SHC_STATE_READING_NONE) but we still have data in the read buffer,
    // it means that multiple commands were concatenated, so we need to run
    // the asynchronous reader again to consume the buffer until we can.
    int state = async_read_context_state(ctx->reader_ctx);
    while (state == SHC_STATE_READING_DONE) {
        shardcache_worker_context_t *wrkctx = ctx->worker_ctx;
        ATOMIC_CAS(wrkctx->busy, 0, 1);

        ctx->hdr = async_read_context_hdr(ctx->reader_ctx);
        ctx->sig_hdr = async_read_context_sig_hdr(ctx->reader_ctx);

        process_request(ctx);
        // if we have output, let's send it
        if (fbuf_used(ctx->output)) {

            int wb = iomux_write(iomux, fd,
                                 fbuf_data(ctx->output),
                                 fbuf_used(ctx->output));

            fbuf_remove(ctx->output, wb);
            if (fbuf_used(ctx->output)) {
                // too much output, let's keep pushing it in a timeout handler
                iomux_callbacks_t *cbs = iomux_callbacks(iomux, fd);
                cbs->mux_output = shardcache_output_handler;
            }
        }

        int i;
        for (i = 0; i < SHARDCACHE_CONNECTION_CONTEX_RECORDS_MAX; i++)
            fbuf_clear(&ctx->records[i]);

        ATOMIC_CAS(wrkctx->busy, 1, 0);

        // we might have received already data for the next command,
        // so let's run the asynchronous reader again to update
        // consume all pending data, if any, and to update the context 
        async_read_context_input_data(NULL, 0, ctx->reader_ctx);

        state = async_read_context_state(ctx->reader_ctx);
    }

    if (state == SHC_STATE_READING_ERR || state == SHC_STATE_AUTH_ERR) {
        // if the asynchronous reader is in error state we want
        // to close the connection, probably an unauthorized or a
        // badly formatted message has been sent by the client
        struct sockaddr_in saddr;
        socklen_t addr_len = sizeof(struct sockaddr_in);
        getpeername(fd, (struct sockaddr *)&saddr, &addr_len);
        SHC_DEBUG("Bad message %02x from %s (%d)\n",
                  ctx->hdr, inet_ntoa(saddr.sin_addr), state);
        iomux_close(iomux, fd);
    }
}

static void
shardcache_eof_handler(iomux_t *iomux, int fd, void *priv)
{
    shardcache_connection_context_t *ctx =
        (shardcache_connection_context_t *)priv;

    close(fd);

    if (ctx) {
        shardcache_worker_context_t *wrkctx = ctx->worker_ctx;
        ATOMIC_DECREMENT(wrkctx->num_fds);
        shardcache_connection_context_destroy(ctx);
    }
}

void *
worker(void *priv)
{
    shardcache_worker_context_t *wrk_ctx = (shardcache_worker_context_t *)priv;
    queue_t *jobs = wrk_ctx->jobs;

    iomux_t *iomux = iomux_create();
    while (ATOMIC_READ(wrk_ctx->leave) == 0) {
        shardcache_connection_context_t *ctx = queue_pop_left(jobs);
        while(ctx) {
            iomux_callbacks_t connection_callbacks = {
                .mux_connection = NULL,
                .mux_input = shardcache_input_handler,
                .mux_eof = shardcache_eof_handler,
                .mux_output = NULL,
                .mux_timeout = NULL,
                .priv = ctx
            };
            ctx->worker_ctx = wrk_ctx;

            iomux_add(iomux, ctx->fd, &connection_callbacks);
            ATOMIC_INCREMENT(wrk_ctx->num_fds);
            ctx = queue_pop_left(jobs);
        }
        pthread_testcancel();
        if (!iomux_isempty(iomux)) {
            struct timeval timeout = { 0, 200000 }; // 200ms
            iomux_run(iomux, &timeout);
        } else {
            // we don't have any filedescriptor to handle in the mux,
            // let's sit for 1 second waiting for the listener thread to wake
            // us up if new filedescriptors arrive
            struct timespec abstime;
            struct timeval now;
            int rc = 0;

            // reset the num_fds
            ATOMIC_SET(wrk_ctx->num_fds, 0);

            rc = gettimeofday(&now, NULL);
            if (rc == 0) {
                abstime.tv_sec = now.tv_sec + 1;
                abstime.tv_nsec = now.tv_usec * 1000;

                pthread_mutex_lock(&wrk_ctx->wakeup_lock);

                pthread_cond_timedwait(&wrk_ctx->wakeup_cond,
                                       &wrk_ctx->wakeup_lock,
                                       &abstime);

                pthread_mutex_unlock(&wrk_ctx->wakeup_lock);
            } else {
                // TODO - Error messsages
            }
        }
    }
    iomux_destroy(iomux);
    return NULL;
}

void *
serve_cache(void *priv)
{
    shardcache_serving_t *serv = (shardcache_serving_t *)priv;

    SHC_NOTICE("Listening on %s (num_workers: %d)",
               serv->me, serv->num_workers);

    if (listen(serv->sock, -1) != 0) {
        SHC_ERROR("Error listening on fd %d: %s",
                  serv->sock, strerror(errno));
        return NULL;
    }

    int idx = 0;
    for (;;) {
        struct sockaddr peer_addr = { 0 };
        socklen_t addr_len = 0;

        pthread_testcancel();

        int fd = accept(serv->sock, &peer_addr, &addr_len);
        if (fd >= 0) {
            // begin the worker slection process
            // first get the next worker in the array
            int index = idx++ % serv->num_workers;
            shardcache_worker_context_t *wrkctx = &serv->workers[index];
            shardcache_worker_context_t *freemost_worker = NULL;
            int cnt = 0;

            // skip over workers who are busy computing/serving a response
            while (ATOMIC_READ(wrkctx->busy) != 0) {

                // update the ponter to the freemost worker, we might need it 
                // if all workers are busy
                if (!freemost_worker)
                    freemost_worker = wrkctx;
                else if (ATOMIC_READ(wrkctx->num_fds) < ATOMIC_READ(freemost_worker->num_fds))
                    freemost_worker = wrkctx;

                wrkctx = &serv->workers[idx++ % serv->num_workers];
                // well ...if we've gone through the whole list and all threads
                // are busy) let's queue this filedescriptor to the one with
                // the least queued  fildescriptors
                if (++cnt == serv->num_workers) {
                    wrkctx = freemost_worker;
                    break;
                }
            }

            // now we have selected a worker, let's see if it makes sense
            // to go ahead
            pthread_testcancel();

            // create and initialize the context for the new connection
            shardcache_connection_context_t *ctx =
                shardcache_connection_context_create(serv->cache, serv->auth, fd);

            queue_push_right(wrkctx->jobs, ctx);
            pthread_mutex_lock(&wrkctx->wakeup_lock);
            pthread_cond_signal(&wrkctx->wakeup_cond);
            pthread_mutex_unlock(&wrkctx->wakeup_lock);
        }
    }

    return NULL;
}

shardcache_serving_t *start_serving(shardcache_t *cache,
                                    const char *auth,
                                    const char *me,
                                    int num_workers,
                                    shardcache_counters_t *counters)
{
    shardcache_serving_t *s = calloc(1, sizeof(shardcache_serving_t));
    s->cache = cache;
    s->me = me;
    s->auth = auth;
    s->num_workers = num_workers;
    s->counters = counters;

    // open the listening socket
    char *brkt = NULL;
    char *addr = strdup(me); // we need a temporary copy to be used by strtok
    char *host = strtok_r(addr, ":", &brkt);
    char *port_string = strtok_r(NULL, ":", &brkt);
    int port = port_string ? atoi(port_string) : SHARDCACHE_PORT_DEFAULT;

    s->sock = open_socket(host, port);
    if (s->sock == -1) {
        fprintf(stderr, "Can't open listening socket %s:%d : %s\n",
                host, port, strerror(errno));
        free(addr);
        free(s);
        return NULL;
    }

    free(addr); // we don't need it anymore

    // create the workers' pool
    s->workers = calloc(num_workers, sizeof(shardcache_worker_context_t));

    int i;
    for (i = 0; i < num_workers; i++) {
        s->workers[i].jobs = queue_create();
        queue_set_free_value_callback(s->workers[i].jobs,
                (queue_free_value_callback_t)shardcache_connection_context_destroy);

        if (s->counters) {
            char varname[256];
            snprintf(varname, sizeof(varname), "worker[%d].numfds", i);
            shardcache_counter_add(s->counters, varname, &s->workers[i].num_fds);
            snprintf(varname, sizeof(varname), "worker[%d].busy", i);
            shardcache_counter_add(s->counters, varname, &s->workers[i].busy);
        }

        pthread_mutex_init(&s->workers[i].wakeup_lock, NULL);
        pthread_cond_init(&s->workers[i].wakeup_cond, NULL);
        pthread_create(&s->workers[i].thread, NULL, worker, &s->workers[i]);
    }

    // and start a background thread to handle incoming connections
    int rc = pthread_create(&s->listener, NULL, serve_cache, s);
    if (rc != 0) {
        fprintf(stderr, "Can't create new thread: %s\n", strerror(errno));
        stop_serving(s);
        return NULL;
    }

    return s;
}

void stop_serving(shardcache_serving_t *s) {
    int i;

    pthread_cancel(s->listener);
    pthread_join(s->listener, NULL);
    SHC_NOTICE("Collecting worker threads (might have to wait until i/o is finished)");
    for (i = 0; i < s->num_workers; i++) {

        if (s->counters) {
            char varname[256];
            snprintf(varname, sizeof(varname), "worker%d::numfds", i);
            shardcache_counter_remove(s->counters, varname);
            snprintf(varname, sizeof(varname), "worker%d::busy", i);
            shardcache_counter_remove(s->counters, varname);
        }

        ATOMIC_INCREMENT(s->workers[i].leave);

        // wake up the worker if slacking
        pthread_mutex_lock(&s->workers[i].wakeup_lock);
        pthread_cond_signal(&s->workers[i].wakeup_cond);
        pthread_mutex_unlock(&s->workers[i].wakeup_lock);

        pthread_join(s->workers[i].thread, NULL);

        queue_destroy(s->workers[i].jobs);

        pthread_mutex_destroy(&s->workers[i].wakeup_lock);
        pthread_cond_destroy(&s->workers[i].wakeup_cond);
    }
    free(s->workers);
    close(s->sock);
    free(s);
}
