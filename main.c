#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MEMORY_BARRIER asm volatile("mfence" ::: "memory")

static void print_usage(const char *name) {
    printf("Usage: %s [PORT] <REMOTE>\n", name);
    printf("Listens to PORT and transfer data between local and REMOTE.\n");
}

enum SocketType { SocketTypeLocal, SocketTypePeer };

static int socket_name_info(int fd, char *host, char *port, enum SocketType tp) {
    struct sockaddr_in6 addr;
    socklen_t addr_len = sizeof(struct sockaddr_in6);
    int n;
    switch (tp) {
        case SocketTypeLocal:
            n = getsockname(fd, (struct sockaddr *)(&addr), &addr_len);
            break;
        default:
            n = getpeername(fd, (struct sockaddr *)(&addr), &addr_len);
            break;
    }
    if (n == -1) {
        return -1;
    }
    return getnameinfo((struct sockaddr *)(&addr), addr_len, host, NI_MAXHOST, port, NI_MAXSERV,
                       NI_NUMERICSERV | NI_NUMERICHOST);
}

static char *split_address(char *forward) {
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

static int get_address(const char *host, const char *port, struct addrinfo **addr) {
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

struct forwardpair {
    int fdin, fdout;
};

static void *transfer(void *ptr) {
    int fdin = ((struct forwardpair *)ptr)->fdin;
    int fdout = ((struct forwardpair *)ptr)->fdout;
    free(ptr);

    int received, sended;
#define BUF_SIZE 1024
    char buf[BUF_SIZE];
    char *addr;

    for (;;) {
        received = recv(fdin, buf, BUF_SIZE, 0);
        if (received < 0) {
            if (errno != EBADF) {
                perror("read() fails:");
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
                    perror("write() fails:");
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

static void start_transfer(int sock, int remote) {
    pthread_t thrd1, thrd2;
    struct forwardpair *pair1 = malloc(sizeof(struct forwardpair));
    struct forwardpair *pair2 = malloc(sizeof(struct forwardpair));
    pair1->fdin = sock;
    pair1->fdout = remote;
    pthread_create(&thrd1, NULL, transfer, pair1);
    pair2->fdin = remote;
    pair2->fdout = sock;
    pthread_create(&thrd2, NULL, transfer, pair2);
}

static int connect_to(struct addrinfo *addr) {
    int sdf = -1;
    struct addrinfo *rp;
    for (rp = addr; rp != NULL; rp = rp->ai_next) {
        sdf = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sdf == -1) continue;
        if ((connect(sdf, rp->ai_addr, rp->ai_addrlen)) != -1) {
            break;
        }
        close(sdf);
    }
    return sdf;
}

static int listen_sock;

void close_listen_sock(void) {
    if (listen_sock >= 0) {
        close(listen_sock);
    }
}

int main(int argc, char *argv[]) {
    int port;
    char *forward;
    switch (argc) {
        case 2:
            port = 0;
            forward = argv[1];
            break;
        case 3:
            if ((port = atoi(argv[1])) < 0) {
                printf("Bad port: %s\n", argv[1]);
                return 1;
            }
            forward = argv[2];
            break;
        default:
            printf("Invalid input\n");
            print_usage(argv[0]);
            return 1;
    }
    char *sep;
    if ((sep = split_address(forward)) == NULL) {
        printf("Invalid FORWARD address: %s\n", forward);
        return 1;
    }
    struct addrinfo *forward_addr_list;
    int n;
    *sep = '\0';
    if ((n = get_address(forward, sep + 1, &forward_addr_list)) < 0) {
        fprintf(stderr, "getaddrinfo() %s:%s: %s\n", forward, sep + 1, gai_strerror(n));
        return 1;
    }

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
        perror("could not create listen socket");
        return 1;
    }

    // bind it to listen to the incoming connections on the created server
    // address, will return -1 on error
    if ((bind(listen_sock, (struct sockaddr *)&server_address, sizeof(server_address))) < 0) {
        perror("could not bind socket");
        return 1;
    }
    atexit(close_listen_sock);
    int wait_size = 128;  // maximum number of waiting clients, after which
                          // dropping begins
    if (listen(listen_sock, wait_size) < 0) {
        perror("could not open socket for listening");
        return 1;
    }
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];
    n = socket_name_info(listen_sock, host, service, SocketTypeLocal);
    if (n == -1) {
        perror("Could not get listened host and port");
        return 1;
    }
    printf("Listen and server at %s:%s\n", host, service);
    // socket address used to store client address
    struct sockaddr_in client_address;
    socklen_t client_address_len = 0;

    for (;;) {
        int sock, remote;
        sock = accept(listen_sock, (struct sockaddr *)&client_address, &client_address_len);
        if (sock < 0) {
            perror("could not open a socket to accept data\n");
            return 1;
        }
        n = socket_name_info(sock, host, service, SocketTypePeer);
        if (n == -1) {
            perror("Could not get host and port");
            close(sock);
            break;
        }
        printf("Client connected: %s:%s\n", host, service);

        remote = connect_to(forward_addr_list);
        if (remote == -1) {
            perror("Could not connect remote");
            return -1;
        }
        start_transfer(sock, remote);
    }

    close(listen_sock);
    return 0;
}
