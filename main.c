/*
 * This file is part of the tcporter distribution (https://github.com/ko-han/tcporter).
 * Copyright (c) 2022 ko-han
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct tp_args {
    uint16_t port;
    int thread;
    struct addrinfo *forward_addr_list;
};

const char *tp_usage =
    "Usage: %s [OPTION] [PORT] <REMOTE>\n"
    "Listens to PORT and transfer data between local and REMOTE.\n"
    "OPTION:\n"
    "  -h    Print this text and exit.\n"
    "  -v    Verbose output.\n"
    "  -q    Quiet output.\n"
    "  -t    Use thread-mode instead of epool-mode.\n";
void tp_print_usage(const char *name) { printf(tp_usage, name); }

const char *tp_strnow() {
    time_t now = time(NULL);
    static char timebuf[1024];
    timebuf[0] = '\0';
    timebuf[1023] = '\0';
    struct tm t;
    localtime_r(&now, &t);
    strftime(timebuf, 1023, "%Y-%m-%d %T %Z", &t);
    return timebuf;
}

enum TP_LOGLVL {
    TP_VERBOSE,
    TP_INFO,
    TP_WARNING,
};
int g_tp_log_lvl = TP_INFO;

#define TP_STRINGSIZE2(x) #x
#define TP_STRINGSIZE(x) TP_STRINGSIZE2(x)
#define TP_LINE_STRING TP_STRINGSIZE(__LINE__)
int __g_tp_log;  // ignore unused variables;
#define TP_LOG(lvl, ...)     \
    if (g_tp_log_lvl <= lvl) \
    __g_tp_log =             \
        fprintf(stderr, "%s ", tp_strnow()) && fprintf(stderr, TP_LINE_STRING " - " __VA_ARGS__)

#define TP_ERRMSG strerror(errno)

#define TP_PANIC(format, ...)                             \
    fprintf(stderr, "%d " format, __LINE__, __VA_ARGS__); \
    exit(EXIT_FAILURE)

/* Don't let caller knowns that tp_malloc and tp_free are macros. */
#define tp_malloc malloc
#define tp_free free

#define TP_BADFD -1
#define TP_CBADFD -1
#define TP_CERR -1
#define TP_CSUCC 0

#define TP_BUFFER_SIZE 4096

#define TP_CB_ERR -1
#define TP_CB_SUCC 0

#define TP_SETPTR(x, y) \
    if (x != NULL) *x = y

int tp_getnameinfo(const struct sockaddr *addr, socklen_t addrlen, char **host, char **port) {
    static char hostbuf[NI_MAXHOST];
    static char portbuf[NI_MAXSERV];
    hostbuf[0] = '\0';
    portbuf[0] = '\0';
    *host = hostbuf;
    *port = portbuf;
    return getnameinfo(addr, addrlen, hostbuf, NI_MAXHOST, portbuf, NI_MAXSERV,
                       NI_NUMERICSERV | NI_NUMERICHOST);
}

enum TPSocketType { TPSocketTypeLocal, TPSocketTypePeer };

int tp_socket_name_info(int fd, char **host, char **port, enum TPSocketType tp) {
    struct sockaddr_in6 addr;
    socklen_t addr_len = sizeof(struct sockaddr_in6);
    *host = NULL;
    *port = NULL;
    int n;
    switch (tp) {
        case TPSocketTypeLocal:
            n = getsockname(fd, (struct sockaddr *)(&addr), &addr_len);
            break;
        case TPSocketTypePeer:
            n = getpeername(fd, (struct sockaddr *)(&addr), &addr_len);
            break;
        default:
            return TP_CERR;
    }
    if (n == TP_CERR) {
        return TP_CERR;
    }
    return tp_getnameinfo((struct sockaddr *)&addr, addr_len, host, port);
}

char *tp_split_address(char *forward) {
    char *s;
    s = strchr(forward, ':');
    if (s == NULL) {
        return NULL;
    }
    if (*(s + 1) == '\0') {
        return NULL;
    }
    return s;
}

int tp_get_address(const char *host, const char *port, struct addrinfo **addr) {
    struct addrinfo hints;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ALL;
    hints.ai_protocol = 0;
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;
    return getaddrinfo(host, port, &hints, addr);
}

int tp_listen(uint16_t port) {
    // socket address used for the server
    struct sockaddr_in6 server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin6_family = AF_INET6;
    // htons: host to network short: transforms a value in host byte
    // ordering format to a short value in network byte ordering format
    server_address.sin6_port = htons(port);
    // htonl: host to network long: same as htons but to long
    server_address.sin6_addr = in6addr_any;

    // create a TCP socket, creation returns -1 on failure
    int listen_sock;
    if ((listen_sock = socket(AF_INET6, SOCK_STREAM, 0)) < 0) {
        TP_LOG(TP_WARNING, "Could not create listen socket: %s\n", TP_ERRMSG);
        return TP_CBADFD;
    }

    // bind it to listen to the incoming connections on the created server
    // address, will return -1 on error
    if ((bind(listen_sock, (struct sockaddr *)&server_address, sizeof(server_address))) < 0) {
        TP_LOG(TP_WARNING, "Could not bind socket: %s\n", TP_ERRMSG);
        return TP_CBADFD;
    }
    static const int wait_size = 128;  // maximum number of waiting clients, after which
                                       // dropping begins
    if (listen(listen_sock, wait_size) < 0) {
        TP_LOG(TP_WARNING, "Could not open socket for listening: %s\n", TP_ERRMSG);
        return TP_CBADFD;
    }
    return listen_sock;
}

int tp_async_connect_to(struct addrinfo *addr) {
    int sdf = TP_CBADFD;
    struct addrinfo *rp;
    for (rp = addr; rp != NULL; rp = rp->ai_next) {
        sdf = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sdf != TP_CBADFD) {
            int flags = fcntl(sdf, F_GETFL);
            if (flags != TP_CERR) {
                if (fcntl(sdf, F_SETFL, flags | O_NONBLOCK) != TP_CERR) {
                    int n = connect(sdf, rp->ai_addr, rp->ai_addrlen);
                    if (n != TP_CERR) break;
                    if (errno == EINPROGRESS) break;
                }
            }
        }
        TP_LOG(TP_VERBOSE, "Connect fails: %s\n", TP_ERRMSG);
        close(sdf);
    }
    return sdf;
}

/***** io_uring implementation *****/

/* Macros for barriers needed by io_uring */
#define io_uring_smp_store_release(p, v) atomic_store_explicit(p, (v), memory_order_release)
#define io_uring_smp_load_acquire(p) atomic_load_explicit(p, memory_order_acquire)
#define IO_URING_WRITE_ONCE(var, val) atomic_store_explicit(var, (val), memory_order_relaxed)
#define IO_URING_READ_ONCE(var) atomic_load_explicit(var, memory_order_relaxed)
#define io_uring_smp_mb() atomic_thread_fence(memory_order_seq_cst)

/*
 * System call wrappers provided since glibc does not yet
 * provide wrappers for io_uring system calls.
 * */
inline int io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}
inline int io_uring_enter(int ring_fd, unsigned int to_submit, unsigned int min_complete,
                          unsigned int flags) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
}

inline int io_uring_register(int fd, unsigned opcode, const void *arg, unsigned nr_args) {
    return (int)syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

/*
 * io_uring interfaces.
 */

/* Submition Queue. */
struct tp_uring_sq {
    _Atomic unsigned *khead;
    _Atomic unsigned *ktail;
    _Atomic unsigned *kring_mask;
    _Atomic unsigned *kring_entries;
    _Atomic unsigned *kflags;
    _Atomic unsigned *kdropped;
    unsigned *array;

    struct io_uring_sqe *sqes;
    unsigned sqe_head;
    unsigned sqe_tail;

    size_t ring_sz;
    void *ring_ptr;
};

/* Complation Queue. */
struct tp_uring_cq {
    _Atomic unsigned *khead;
    _Atomic unsigned *ktail;
    _Atomic unsigned *kring_mask;
    _Atomic unsigned *kring_entries;
    _Atomic unsigned *kflags;
    _Atomic unsigned *koverflow;

    struct io_uring_cqe *cqes;

    size_t ring_sz;
    void *ring_ptr;
};

struct tp_uring {
    int ring_fd;
    unsigned flags;
    unsigned features;
    struct tp_uring_sq sq;
    struct tp_uring_cq cq;
    int kmaxconn;
    int to_submit;
};

/*
 * Closing and free the io_uring, associative memories, meanwhile would be unmap appropriately.
 */
inline void tp_uring_close_free(struct tp_uring *uring) {
    if (uring->sq.ring_ptr != NULL) {
        munmap(uring->sq.ring_ptr, uring->sq.ring_sz);
    }
    if (uring->cq.ring_ptr != NULL && uring->cq.ring_ptr != uring->sq.ring_ptr) {
        munmap(uring->cq.ring_ptr, uring->cq.ring_sz);
    }
    if (uring->ring_fd > 0) {
        close(uring->ring_fd);
    }
    free(uring);
}

/*
 * Adjust the maxconnections into io_uring entries.
 * Negative or zero maxconnection will be adjusted into io_uring max sq entries.
 */
inline int _tp_uring_entries(int maxconnection) {
    static const int maxvalue = 32768;  // max size of sq entryies.
    int r = maxconnection * 2;
    if (r > maxvalue || r <= 0) {
        return maxvalue;
    }
    return r;
}

/*
 * Creates an io_uring and mmap it appropriately.
 */
struct tp_uring *tp_create_uring(int maxconnection) {
    struct tp_uring *uring = (struct tp_uring *)tp_malloc(sizeof(struct tp_uring));
    memset(uring, 0, sizeof(struct tp_uring));
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    // Start a kernel thread to perform submission queue polling.
    // With this enabled, we no longer has to call io_uring_enter to submit IO.
    p.flags = IORING_SETUP_SQPOLL;

    uring->ring_fd = io_uring_setup(_tp_uring_entries(maxconnection), &p);
    uring->kmaxconn = maxconnection;
    if (uring->ring_fd < 0) {
        goto FAIL;
    }
    /*
     * io_uring communication happens via 2 shared kernel-user space ring
     * buffers, which can be jointly mapped with a single mmap() call in
     * kernels >= 5.4.
     */
    uring->sq.ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    uring->cq.ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    /* Rather than check for kernel version, the recommended way is to
     * check the features field of the io_uring_params structure, which is a
     * bitmask. If IORING_FEAT_SINGLE_MMAP is set, we can do away with the
     * second mmap() call to map in the completion ring separately.
     */
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        if (uring->cq.ring_sz > uring->sq.ring_sz) uring->cq.ring_sz = uring->sq.ring_sz;
        uring->cq.ring_sz = uring->sq.ring_sz;
    }
    /* Map in the submission and completion queue ring buffers.
     *  Kernels < 5.4 only map in the submission queue, though.
     */
    uring->sq.ring_ptr = mmap(NULL, uring->sq.ring_sz, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_POPULATE, uring->ring_fd, IORING_OFF_SQ_RING);
    if (uring->sq.ring_ptr == MAP_FAILED) {
        uring->sq.ring_ptr = NULL;
        goto FAIL;
    }
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        uring->cq.ring_ptr = uring->sq.ring_ptr;
    } else {
        /* Map in the completion queue ring buffer in older kernels separately */
        uring->cq.ring_ptr = mmap(NULL, uring->cq.ring_sz, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE, uring->ring_fd, IORING_OFF_CQ_RING);
        if (uring->cq.ring_ptr == MAP_FAILED) {
            uring->cq.ring_ptr = NULL;
            goto FAIL;
        }
    }

    /* Save useful fields for later easy reference */
    uring->sq.khead = uring->sq.ring_ptr + p.sq_off.head;
    uring->sq.ktail = uring->sq.ring_ptr + p.sq_off.tail;
    uring->sq.kring_mask = uring->sq.ring_ptr + p.sq_off.ring_mask;
    uring->sq.kring_entries = uring->sq.ring_ptr + p.sq_off.ring_entries;
    uring->sq.kflags = uring->sq.ring_ptr + p.sq_off.flags;
    uring->sq.kdropped = uring->sq.ring_ptr + p.sq_off.dropped;
    uring->sq.array = uring->sq.ring_ptr + p.sq_off.array;

    size_t size;
    /* Map in the submission queue entries array */
    uring->sq.kring_entries =
        mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_POPULATE, uring->ring_fd, IORING_OFF_SQES);
    if (uring->sq.kring_entries == MAP_FAILED) {
        uring->sq.kring_entries = NULL;
        goto FAIL;
    }
    /* Save useful fields for later easy reference */
    uring->cq.khead = uring->cq.ring_ptr + p.cq_off.head;
    uring->cq.ktail = uring->cq.ring_ptr + p.cq_off.tail;
    uring->cq.kring_mask = uring->cq.ring_ptr + p.cq_off.ring_mask;
    uring->cq.kring_entries = uring->cq.ring_ptr + p.cq_off.ring_entries;
    uring->cq.koverflow = uring->cq.ring_ptr + p.cq_off.overflow;
    uring->cq.cqes = uring->cq.ring_ptr + p.cq_off.cqes;

    uring->flags = p.flags;
    uring->features = p.features;

    io_uring_smp_mb();  // make above changes could seen by all processes.
    return uring;
FAIL:
    tp_uring_close_free(uring);
    return NULL;
}

/*
 * Returns true if we're not using SQ thread (thus nobody submits but us)
 * or if IORING_SQ_NEED_WAKEUP is set, so submit thread must be explicitly
 * awakened. For the latter case, we set the thread wakeup flag.
 */
bool tp_uring_sq_needs_enter(struct tp_uring *ring) {
    if (!(ring->flags & IORING_SETUP_SQPOLL)) return true;
    /*
     * Ensure the kernel can see the store to the SQ tail before we read
     * the flags.
     */
    io_uring_smp_mb();
    return IO_URING_READ_ONCE(ring->sq.kflags) & IORING_SQ_NEED_WAKEUP;
}

/*
 * Returns the number of I/Os success consumed. -1 on error.
 */
int tp_uring_wakeup_sq_if_needed(struct tp_uring *ring) {
    if (tp_uring_sq_needs_enter(ring)) {
        return io_uring_enter(ring->ring_fd, ring->to_submit, 0, IORING_ENTER_SQ_WAKEUP);
    }
    return 0;
}

int tp_uring_create_buffer(struct tp_uring *ring) {
    struct iovec *vec;
    unsigned size;
    return io_uring_register(ring->ring_fd, IORING_REGISTER_BUFFERS, vec, size);
}

/***** Epoll implementation *****/

struct tp_ringbuf {
    int16_t roffset;
    int16_t woffset;
    char buf[TP_BUFFER_SIZE];
};

int tp_ringbuf_read_ptr(struct tp_ringbuf *buf, char **start) {
    int filled = buf->roffset - buf->woffset;
    if (filled < buf->woffset) {
        memcpy(buf->buf, buf->buf + buf->woffset, filled);
        buf->roffset = filled;
        buf->woffset = 0;
    }
    int size = TP_BUFFER_SIZE - buf->roffset;
    *start = buf->buf + buf->roffset;
    return size;
}

int tp_ringbuf_write_ptr(struct tp_ringbuf *buf, char **start) {
    int filled = buf->roffset - buf->woffset;
    *start = buf->buf + buf->woffset;
    return filled;
}

// EPoll Connection.
struct tp_epc {
    int local;
    int remote;
    struct tp_ringbuf localbuf;
    struct tp_ringbuf remotebuf;
};

// EPoll Entry.
struct tp_epe {
    unsigned flags;
    struct tp_epc *conn;
};

enum TP_EPOLL_FLAG {
    TP_EPOLL_LOCAL = 1u,
    TP_EPOLL_REMOTE = 1u << 1,
};

struct tp_epe_list {
    struct tp_epe entry;
    struct tp_epe_list *next;
};
struct {
    struct tp_epe_list *list;
    int len;
} g_tp_epe_list;

struct tp_epc_list {
    struct tp_epc conn;
    struct tp_epc_list *next;
};
struct {
    struct tp_epc_list *list;
    int len;
} g_tp_epc_list;

void _tp_epc_init(struct tp_epc_list *list) {
    list->conn.local = TP_BADFD;
    list->conn.remote = TP_BADFD;
    list->conn.localbuf.roffset = 0;
    list->conn.localbuf.woffset = 0;
    list->conn.remotebuf.roffset = 0;
    list->conn.remotebuf.woffset = 0;
    list->next = NULL;
}

struct tp_epc *tp_new_epc() {
    struct tp_epc *c;
    struct tp_epc_list *list;
    if (g_tp_epc_list.len > 0 && g_tp_epc_list.list != NULL) {
        assert(g_tp_epc_list.list->next != NULL);
        c = (struct tp_epc *)g_tp_epc_list.list->next;
        g_tp_epc_list.list->next = g_tp_epc_list.list->next->next;
        g_tp_epc_list.len--;
        return c;
    }
    list = (struct tp_epc_list *)tp_malloc(sizeof(struct tp_epc_list));
    _tp_epc_init(list);
    return (struct tp_epc *)list;
}

void tp_free_epc(struct tp_epc *conn) {
    if (g_tp_epc_list.list == NULL) {
        g_tp_epc_list.list = (struct tp_epc_list *)tp_malloc(sizeof(struct tp_epc_list));
        g_tp_epc_list.len = 0;
        g_tp_epc_list.list->next = NULL;
    }
    struct tp_epc_list *right = g_tp_epc_list.list->next;
    struct tp_epc_list *left = (struct tp_epc_list *)conn;
    if (g_tp_epc_list.len > 1024) {
        tp_free(left);
        return;
    }
    left->next = right;
    g_tp_epc_list.list->next = left;
    g_tp_epc_list.len++;
    _tp_epc_init(left);
}

void tp_epe_init_epc(struct tp_epe *c) {
    if (c->conn == NULL) {
        c->conn = tp_new_epc();
    }
}

void _tp_epe_list_init(struct tp_epe_list *list) {
    list->entry.conn = NULL;
    list->entry.flags = 0;
    list->next = NULL;
}

struct tp_epe *tp_new_epe() {
    struct tp_epe *c;
    struct tp_epe_list *list;
    if (g_tp_epe_list.list != NULL && g_tp_epe_list.len > 0) {
        assert(g_tp_epe_list.list->next != NULL);
        c = (struct tp_epe *)g_tp_epe_list.list->next;
        g_tp_epe_list.list->next = g_tp_epe_list.list->next->next;
        g_tp_epe_list.len--;
        return c;
    }
    list = (struct tp_epe_list *)tp_malloc(sizeof(struct tp_epe_list));
    _tp_epe_list_init(list);
    return (struct tp_epe *)list;
}

void tp_free_epe(struct tp_epe *entry) {
    if (g_tp_epe_list.list == NULL) {
        g_tp_epe_list.list = (struct tp_epe_list *)tp_malloc(sizeof(struct tp_epe_list));
        g_tp_epe_list.len = 0;
        g_tp_epe_list.list->next = NULL;
    }
    struct tp_epe_list *mold = g_tp_epe_list.list->next;
    struct tp_epe_list *mnew = (struct tp_epe_list *)entry;
    _tp_epe_list_init(mnew);
    if (g_tp_epe_list.len > 1024) {
        tp_free(mnew);
        return;
    }
    mnew->next = mold;
    g_tp_epe_list.list->next = mnew;
    g_tp_epe_list.len++;
}

/* Manages all the epoll entries. */
struct tp_epoll {
    int pollfd;
    int listenfd;
    int cap;
    int len;
    struct addrinfo *forward_addr_list;
    struct epoll_event *events;
    int maxevents;
};

// EPoll EVentS.
struct tp_epevs {
    struct epoll_event *events;
    int size;
};

void tp_free_epoll(struct tp_epoll *poll) {
    if (poll->listenfd > 0) {
        close(poll->listenfd);
    }
    if (poll->pollfd > 0) {
        close(poll->pollfd);
    }
    free(poll->events);
    free(poll);
}

struct tp_epoll *tp_create_epoll(int cap, int listenfd) {
    struct tp_epoll *poll = (struct tp_epoll *)tp_malloc(sizeof(struct tp_epoll));
    memset(poll, 0, sizeof(struct tp_epoll));
    int maxevents = cap * 2 + 1;
    poll->events = (struct epoll_event *)tp_malloc(sizeof(struct epoll_event) * maxevents);
    int pollfd = epoll_create(maxevents);
    if (pollfd == TP_CBADFD) {
        TP_LOG(TP_VERBOSE, "Create epoll fails: %s\n", TP_ERRMSG);
        tp_free_epoll(poll);
        return NULL;
    }
    struct tp_epe *entry = tp_new_epe();
    tp_epe_init_epc(entry);
    entry->conn->local = listenfd;
    entry->flags = TP_EPOLL_LOCAL;
    struct epoll_event ev;
    ev.data.ptr = entry;
    ev.events = EPOLLIN;
    if (epoll_ctl(pollfd, EPOLL_CTL_ADD, listenfd, &ev) != 0) {
        TP_LOG(TP_VERBOSE, "poll register listener fails: %s\n", TP_ERRMSG);
        close(pollfd);
        tp_free_epoll(poll);
        return NULL;
    }

    poll->cap = cap;
    poll->len = 0;
    poll->listenfd = listenfd;
    poll->pollfd = pollfd;
    poll->maxevents = maxevents;
    return poll;
}

void tp_close_epoll(struct tp_epoll *poll) {
    if (poll->pollfd >= 0) {
        close(poll->pollfd);
    }
    tp_free_epoll(poll);
}

int tp_epoll_register_conn(struct tp_epoll *poll, int local, int remote) {
    if (poll->len == poll->cap) {
        TP_LOG(TP_WARNING, "Pool full, close %d,%d\n", local, remote);
        return TP_CERR;
    }
    struct epoll_event ev;
    struct tp_epe *c0, *c1;
    c0 = tp_new_epe();
    tp_epe_init_epc(c0);
    c0->conn->local = local;
    c0->conn->remote = remote;
    c0->flags = TP_EPOLL_LOCAL;

    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.ptr = c0;
    if (epoll_ctl(poll->pollfd, EPOLL_CTL_ADD, local, &ev) != 0) {
        return TP_CERR;
    }
    c1 = tp_new_epe();
    c1->flags = TP_EPOLL_REMOTE;
    c1->conn = c0->conn;
    ev.data.ptr = c1;
    if (epoll_ctl(poll->pollfd, EPOLL_CTL_ADD, remote, &ev) != 0) {
        epoll_ctl(poll->pollfd, EPOLL_CTL_DEL, local, &ev);
        return TP_CERR;
    }
    poll->len++;
    assert(poll->len <= poll->cap);
    return 0;
}

void _tp_epoll_conn_extract(struct tp_epe *entry, int *fdptr, int *ofdptr,
                            struct tp_ringbuf **rbufptr, struct tp_ringbuf **wbufptr,
                            int **closefd) {
    if (entry->flags & TP_EPOLL_LOCAL) {
        TP_SETPTR(fdptr, entry->conn->local);
        TP_SETPTR(ofdptr, entry->conn->remote);
        TP_SETPTR(rbufptr, &entry->conn->localbuf);
        TP_SETPTR(wbufptr, &entry->conn->remotebuf);
        TP_SETPTR(closefd, &entry->conn->local);
    } else if (entry->flags & TP_EPOLL_REMOTE) {
        TP_SETPTR(fdptr, entry->conn->remote);
        TP_SETPTR(ofdptr, entry->conn->local);
        TP_SETPTR(rbufptr, &entry->conn->remotebuf);
        TP_SETPTR(wbufptr, &entry->conn->localbuf);
        TP_SETPTR(closefd, &entry->conn->remote);
    } else {
        TP_PANIC("Bad epoll entry flags: %d\n", entry->flags);
    }
}

int tp_epoll_close_conn(struct tp_epoll *poll, struct tp_epe *entry) {
    int fd, ofd, *closefd;
    _tp_epoll_conn_extract(entry, &fd, &ofd, NULL, NULL, &closefd);
    struct tp_epc *conn = entry->conn;
    tp_free_epe(entry);
    if (ofd == TP_BADFD) {
        tp_free_epc(conn);
        poll->len--;
        TP_LOG(TP_INFO, "Client closed. %d clients connected\n", poll->len);
    }
    TP_LOG(TP_VERBOSE, "Close fd: %d\n", fd);
    struct epoll_event ev;  // events is ignored when EPOLL_CTL_DEL;
    int n = epoll_ctl(poll->pollfd, EPOLL_CTL_DEL, fd, &ev);
    close(fd);
    *closefd = TP_BADFD;
    return n;
}

struct tp_epevs tp_epoll_wait(struct tp_epoll *poll) {
    static const int timeout = 10;  // 10ms
    int size = epoll_wait(poll->pollfd, poll->events, poll->maxevents, timeout);
    struct tp_epevs events;
    events.events = poll->events;
    events.size = size;
    return events;
}

int _tp_epoll_accept_callback(struct tp_epoll *poll) {
    TP_LOG(TP_VERBOSE, "Handle accepting: %d\n", poll->listenfd);
    struct sockaddr_in client_address;
    socklen_t client_address_len = sizeof(struct sockaddr_in);
    int fd = accept(poll->listenfd, &client_address, &client_address_len);
    char *host, *port;
    int n = tp_socket_name_info(fd, &host, &port, TPSocketTypePeer);
    if (n != 0) {
        TP_LOG(TP_WARNING, "Could not get host and port: %s, %s\n", gai_strerror(n), TP_ERRMSG);
        close(fd);
        return 0;
    }
    int rfd = tp_async_connect_to(poll->forward_addr_list);
    if (rfd == TP_CBADFD) {
        close(fd);
        TP_LOG(TP_WARNING, "Could not connect to remote: %s\n", TP_ERRMSG);
        return 0;
    }
    if (tp_epoll_register_conn(poll, fd, rfd) != 0) {
        TP_LOG(TP_WARNING, "Could not join to the queue: epoll: %s\n", TP_ERRMSG);
        close(fd);
        close(rfd);
        return 0;
    }

    TP_LOG(TP_INFO, "Client established: %d,%d %s:%s. %d clients connected\n", fd, rfd, host, port,
           poll->len);
    return 0;
}

int _tp_epoll_read_callback(struct tp_epoll *poll, struct tp_epe *entry) {
    int fd, ofd;
    struct tp_ringbuf *buf;
    _tp_epoll_conn_extract(entry, &fd, &ofd, &buf, NULL, NULL);

    if (ofd == TP_BADFD) {
        TP_LOG(TP_VERBOSE, "Other side closed, read stopping. %d\n", fd);
        return 0;
    }
    int size, maxsize;
    char *start = NULL;
    maxsize = tp_ringbuf_read_ptr(buf, &start);
    if (maxsize == 0) {
        TP_LOG(TP_VERBOSE, "Read ringbuffer full: %d\n", fd);
        return 0;
    }
    size = recv(fd, start, maxsize, MSG_DONTWAIT);
    if (size == 0) {
        return TP_CB_ERR;
    }
    if (size < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        TP_LOG(TP_VERBOSE, "Recv fails: %d, %s\n", fd, TP_ERRMSG);
        return TP_CB_ERR;
    }
    buf->roffset += size;
    return 0;
}

int _tp_epoll_write_callback(struct tp_epoll *poll, struct tp_epe *entry) {
    int fd, ofd;
    struct tp_ringbuf *buf;
    _tp_epoll_conn_extract(entry, &fd, &ofd, NULL, &buf, NULL);

    int size, maxsize;
    char *start = NULL;
    maxsize = tp_ringbuf_write_ptr(buf, &start);
    if (maxsize == 0) {
        if (ofd == TP_BADFD) {  // Other side closed, stopping write.
            return TP_CB_ERR;
        }
        return 0;
    }
    size = send(fd, start, maxsize, MSG_DONTWAIT);
    if (size <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        TP_LOG(TP_WARNING, "Send fails: %s", TP_ERRMSG);
        return TP_CB_ERR;
    }
    buf->woffset += size;
    return 0;
}

int tp_epoll_callback(struct tp_epoll *poll, struct tp_epe *entry, uint32_t mask) {
    int fd;
    _tp_epoll_conn_extract(entry, &fd, NULL, NULL, NULL, NULL);
    if (fd == TP_BADFD) {  // already closed
        return TP_CB_SUCC;
    }
    if (fd == poll->listenfd) {
        return _tp_epoll_accept_callback(poll);
    }

    if (mask & EPOLLHUP) {
        return TP_CB_ERR;
    }

    if (mask & EPOLLERR) {
        return TP_CB_ERR;
    }

    if (mask & EPOLLIN) {  // readable
        int n = _tp_epoll_read_callback(poll, entry);
        if (n != 0) {
            return TP_CB_ERR;
        }
    }

    if (mask & EPOLLOUT) {
        int n = _tp_epoll_write_callback(poll, entry);
        if (n != 0) {
            return TP_CB_ERR;
        }
    }
    return TP_CB_SUCC;
}

int tp_epoll_main(struct tp_args args) {
    int listenfd = tp_listen(args.port);
    if (listenfd == TP_BADFD) {
        TP_PANIC("Listen port fails: %s\n", TP_ERRMSG);
        return 1;
    }
    struct tp_epoll *poll = tp_create_epoll(1024, listenfd);
    if (poll == NULL) {
        TP_PANIC("Could not setup manager: %s\n", TP_ERRMSG);
        return 1;
    }
    char *host, *port;
    int n;
    if ((n = tp_socket_name_info(poll->listenfd, &host, &port, TPSocketTypeLocal)) != 0) {
        TP_LOG(TP_WARNING, "Could not get listen address: %s, %s\n", gai_strerror(n), TP_ERRMSG);
        tp_free_epoll(poll);
        return 1;
    }
    poll->forward_addr_list = args.forward_addr_list;
    TP_LOG(TP_INFO, "Listen at %s:%s\n", host, port);
    struct tp_epevs events;
    for (;;) {
        events = tp_epoll_wait(poll);
        if (events.size < 0) {
            TP_LOG(TP_WARNING, "Pool wait fails: %s\n", TP_ERRMSG);
            return 1;
        }
        if (events.size == 0) {
            continue;
        }
        int i;
        for (i = 0; i < events.size; i++) {
            int ret;
            struct tp_epe *entry = (struct tp_epe *)(events.events[i].data.ptr);
            ret = tp_epoll_callback(poll, entry, events.events[i].events);
            if (ret != TP_CB_SUCC) {
                ret = tp_epoll_close_conn(poll, entry);
                if (ret != TP_CSUCC) {
                    TP_LOG(TP_WARNING, "Close connection fails: %s\n", TP_ERRMSG);
                    return 1;
                }
            }
        }
    }
    return 0;
}

/***** Thread Implementation *****/

struct forwardpair {
    int fdin, fdout;
};

void *tp_transfer_background(void *ptr) {
    int fdin = ((struct forwardpair *)ptr)->fdin;
    int fdout = ((struct forwardpair *)ptr)->fdout;
    tp_free(ptr);

    int received, sended;
    char buf[TP_BUFFER_SIZE];
    char *addr;

    for (;;) {
        received = recv(fdin, buf, TP_BUFFER_SIZE, 0);
        if (received < 0) {
            if (errno != EBADF) {
                TP_LOG(TP_WARNING, "read() fails: %s\n", TP_ERRMSG);
            }
            break;
        }
        if (received == 0) {
            break;
        }
        addr = buf;
        for (; received > 0;) {
            sended = send(fdout, addr, received, 0);
            if (sended < 0) {
                if (errno != EBADF) {
                    TP_LOG(TP_WARNING, "write() fails: %s\n", TP_ERRMSG);
                }
                goto EXIT;
            }
            received = received - sended;
            addr += sended;
        }
    }
EXIT:
    close(fdin);
    close(fdout);
    return NULL;
}

void tp_start_transfer(int sock, int remote) {
    pthread_t thrd1, thrd2;
    struct forwardpair *pair1 = tp_malloc(sizeof(struct forwardpair));
    struct forwardpair *pair2 = tp_malloc(sizeof(struct forwardpair));
    pair1->fdin = sock;
    pair1->fdout = remote;
    pthread_create(&thrd1, NULL, tp_transfer_background, pair1);
    pair2->fdin = remote;
    pair2->fdout = sock;
    pthread_create(&thrd2, NULL, tp_transfer_background, pair2);
    // free forwardpair in the thread job;
}

int tp_connect_to(struct addrinfo *addr) {
    int sdf = TP_CBADFD;
    struct addrinfo *rp;
    for (rp = addr; rp != NULL; rp = rp->ai_next) {
        sdf = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sdf == TP_CBADFD) continue;
        if ((connect(sdf, rp->ai_addr, rp->ai_addrlen)) != TP_CERR) {
            break;
        }
        close(sdf);
    }
    return sdf;
}

int tp_thread_main(struct tp_args args) {
    int listenfd;
    if ((listenfd = tp_listen(args.port)) == TP_CERR) {
        TP_LOG(TP_WARNING, "Could not listen: %s\n", TP_ERRMSG);
        return 1;
    }
    char *host, *port;
    int n = tp_socket_name_info(listenfd, &host, &port, TPSocketTypeLocal);
    if (n == TP_CERR) {
        TP_LOG(TP_WARNING, "Could not get listened host and port: %s\n", TP_ERRMSG);
        close(listenfd);
        return 1;
    }
    TP_LOG(TP_INFO, "Listen and server at %s:%s\n", host, port);
    // socket address used to store client address
    struct sockaddr_in client_address;
    socklen_t client_address_len = 0;

    for (;;) {
        int sock, remote;
        sock = accept(listenfd, (struct sockaddr *)&client_address, &client_address_len);
        if (sock < 0) {
            TP_LOG(TP_WARNING, "could not open a socket to accept data: %s\n", TP_ERRMSG);
            close(listenfd);
            return 1;
        }
        n = tp_socket_name_info(sock, &host, &port, TPSocketTypePeer);
        if (n == TP_CERR) {
            TP_LOG(TP_WARNING, "Could not get host and port %s\n", TP_ERRMSG);
            close(sock);
            continue;
        }
        TP_LOG(TP_INFO, "Client connected: %s:%s\n", host, port);

        remote = tp_connect_to(args.forward_addr_list);
        if (remote == TP_CERR) {
            TP_LOG(TP_WARNING, "Could not connect remote: %s\n", TP_ERRMSG);
            close(sock);
            continue;
        }
        tp_start_transfer(sock, remote);
    }
    close(listenfd);
    return 0;
}

const char *_tp_get_option_arguments(int argc, char *argv[], int i) {
    if (i + 1 < argc) {
        char *value = argv[i + 1];
        if (value[0] == '-') {
            return NULL;
        }
        return value;
    }
    return NULL;
}

int tp_args_parse(int argc, char *argv[], struct tp_args *args) {
    char *listen_port = NULL, *fowardaddr = NULL;
    int port = 0, thread = 0;
    int i;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i];

        if (arg[0] == '-') {
            if (strcmp(arg, "-v") == 0) {
                g_tp_log_lvl = TP_VERBOSE;
                continue;
            } else if (strcmp(arg, "-q") == 0) {
                g_tp_log_lvl = TP_WARNING;
                continue;
            } else if (strcmp(arg, "-h") == 0) {
                tp_print_usage(argv[0]);
                exit(EXIT_SUCCESS);
            } else if (strcmp(arg, "-t") == 0) {
                thread = 1;
                continue;
            } else {
                arg--;
                TP_PANIC("Unknown option: %s\n", arg);
                return 1;
            }
        }
        if (listen_port == NULL) {
            listen_port = arg;
        } else if (fowardaddr == NULL) {
            fowardaddr = arg;
        } else {
            TP_PANIC("Too many arguments, expect %d\n", 2);
            return 1;
        }
    }
    if (listen_port == NULL) {
        TP_PANIC("Forward address not provides%s\n", ".");
        return 1;
    }
    if (fowardaddr == NULL) {
        fowardaddr = listen_port;
        listen_port = NULL;
    }
    if (listen_port != NULL) {
        if ((port = atoi(listen_port)) < 0) {
            TP_PANIC("Bad port: %s\n", listen_port);
            return 1;
        }
        if (port < 0 && port > (1 << 16) - 1) {
            TP_PANIC("Bad port: %s\n", listen_port);
            return 1;
        }
    }
    char *sep;
    if ((sep = tp_split_address(fowardaddr)) == NULL) {
        TP_PANIC("Invalid FORWARD address: %s\n", fowardaddr);
        return 1;
    }
    struct addrinfo *forward_addr_list;
    char old = *sep;
    *sep = '\0';
    int n;
    if ((n = tp_get_address(fowardaddr, sep + 1, &forward_addr_list)) != 0) {
        TP_PANIC("getaddrinfo() %s:%s: %s\n", fowardaddr, sep + 1, gai_strerror(n));
        *sep = old;
        return 1;
    }
    if (forward_addr_list == NULL) {
        TP_PANIC("getaddrinfo() results empty%s\n", ".");
        return 1;
    }
    *sep = old;
    args->forward_addr_list = forward_addr_list;
    args->port = (uint16_t)port;
    args->thread = thread;
    return 0;
}

int main(int argc, char *argv[]) {
    struct tp_args args;
    int n = tp_args_parse(argc, argv, &args);
    if (n != 0) {
        return n;
    }
    if (args.thread) {
        return tp_thread_main(args);
    }
    return tp_epoll_main(args);
}
