#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

/* ===== OpenSSL ===== */
#include <openssl/ssl.h>
#include <openssl/err.h>

#define MAXLINE 4096
#define MAXUSERS 256

typedef struct {
    char name[64];
    char ip[64];
    int  port;
} Peer;

/* ===== Global ===== */
static int srv_fd = -1;
static SSL_CTX *srv_ctx = NULL;
static SSL *srv_ssl = NULL;

/* P2P TLS */
static SSL_CTX *p2p_srv_ctx = NULL;
static SSL_CTX *p2p_cli_ctx = NULL;

static char my_name[64] = {0};
static int  my_port = 0;
static Peer peers[MAXUSERS];
static int  peer_cnt = 0;

/* ===== Utility ===== */
static void die(const char *m){ perror(m); exit(1); }
static void ssl_die(void){ ERR_print_errors_fp(stderr); exit(1); }

static int ssl_send(const char *fmt, ...) {
    char buf[MAXLINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return SSL_write(srv_ssl, buf, strlen(buf));
}

static int ssl_recv_line(SSL *ssl, char *buf, size_t cap) {
    size_t i = 0;
    while (i+1 < cap) {
        char c;
        int r = SSL_read(ssl, &c, 1);
        if (r <= 0) return -1;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = 0;
    return 1;
}

/* ===== Read LIST ===== */
static int read_list(void) {
    char line[MAXLINE];
    peer_cnt = 0;

    ssl_recv_line(srv_ssl, line, sizeof(line));
    printf("%s\n", line);           // balance

    ssl_recv_line(srv_ssl, line, sizeof(line));
    printf("%s\n", line);           // public key

    ssl_recv_line(srv_ssl, line, sizeof(line));
    int n = atoi(line);
    printf("%d\n", n);

    for (int i = 0; i < n; i++) {
        ssl_recv_line(srv_ssl, line, sizeof(line));
        printf("%s\n", line);
        sscanf(line, "%[^#]#%[^#]#%d",
               peers[peer_cnt].name,
               peers[peer_cnt].ip,
               &peers[peer_cnt].port);
        peer_cnt++;
    }
    return 0;
}

/* ===== TCP connect ===== */
static int tcp_connect(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, ip, &sa.sin_addr);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0)
        die("connect");
    return fd;
}

/* ===== P2P Listener ===== */
void *p2p_listener(void *arg) {
    int port = *(int*)arg;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return NULL;
    }
    if (listen(fd, 10) < 0) {
        close(fd);
        return NULL;
    }

    while (1) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) continue;
        
        SSL *ssl = SSL_new(p2p_srv_ctx);
        if (!ssl) {
            close(cfd);
            continue;
        }
        SSL_set_fd(ssl, cfd);

        if (SSL_accept(ssl) <= 0) {
            SSL_free(ssl);
            close(cfd);
            continue;
        }

        char buf[MAXLINE];
        int n = SSL_read(ssl, buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = 0;
            // Remove trailing newline if present
            if (n > 0 && buf[n-1] == '\n') buf[n-1] = 0;
            if (n > 1 && buf[n-2] == '\r') buf[n-2] = 0;
            
            // Forward to server (thread-safe: SSL_write is atomic)
            if (srv_ssl) {
                char msg[MAXLINE];
                int len = strlen(buf);
                if (len < sizeof(msg) - 2) {
                    snprintf(msg, sizeof(msg), "%s\n", buf);
                    SSL_write(srv_ssl, msg, strlen(msg));
                }
            }
        }

        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(cfd);
    }
    
    close(fd);
    return NULL;
}

/* ===== P2P Transfer ===== */
static void p2p_transfer(const char *ip, int port, const char *msg) {
    int fd = tcp_connect(ip, port);
    SSL *ssl = SSL_new(p2p_cli_ctx);
    SSL_set_fd(ssl, fd);

    if (SSL_connect(ssl) <= 0) ssl_die();

    SSL_write(ssl, msg, strlen(msg));

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
}

/* ===== Main ===== */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <server_ip> <server_port>\n", argv[0]);
        return 1;
    }

    /* === OpenSSL init === */
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    srv_ctx = SSL_CTX_new(TLS_client_method());
    if (!srv_ctx) ssl_die();

    SSL_CTX_set_verify(srv_ctx, SSL_VERIFY_NONE, NULL);

    p2p_srv_ctx = SSL_CTX_new(TLS_server_method());
    p2p_cli_ctx = SSL_CTX_new(TLS_client_method());

    SSL_CTX_use_certificate_file(p2p_srv_ctx, "cert/server.crt", SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(p2p_srv_ctx, "cert/server.key", SSL_FILETYPE_PEM);
    SSL_CTX_set_verify(p2p_cli_ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_verify(p2p_srv_ctx, SSL_VERIFY_NONE, NULL);

    /* === connect server === */
    srv_fd = tcp_connect(argv[1], atoi(argv[2]));
    srv_ssl = SSL_new(srv_ctx);
    SSL_set_fd(srv_ssl, srv_fd);

    if (SSL_connect(srv_ssl) <= 0) ssl_die();
    printf("Connected to server (TLS enabled)\n");

    /* === interactive === */
    char cmd[MAXLINE];
    while (1) {
        printf(">");
        fflush(stdout);
        if (!fgets(cmd, sizeof(cmd), stdin)) break;
        cmd[strcspn(cmd,"\n")] = 0;

        ssl_send("%s\n", cmd);

        // Check command type and handle response accordingly
        if (strcmp(cmd, "List") == 0) {
            // List command returns LIST format (multi-line)
            read_list();
        }
        else if (strcmp(cmd, "Exit") == 0) {
            // Exit command returns single line "Bye\n"
            char buf[MAXLINE];
            if (ssl_recv_line(srv_ssl, buf, sizeof(buf)) > 0) {
                printf("%s\n", buf);
            }
            break;
        }
        else if (strncmp(cmd, "REGISTER#", 9) == 0) {
            // REGISTER returns single line response
            char buf[MAXLINE];
            if (ssl_recv_line(srv_ssl, buf, sizeof(buf)) > 0) {
                printf("%s\n", buf);
            }
        }
        else if (strchr(cmd, '#') && strchr(strchr(cmd, '#') + 1, '#')) {
            // TRANSFER: A#amt#B (two #)
            // Server sends "Transfer OK!\n" to sender after processing
            char to[64];
            sscanf(cmd, "%*[^#]#%*[^#]#%s", to);
            for (int i = 0; i < peer_cnt; i++) {
                if (strcmp(peers[i].name, to) == 0) {
                    p2p_transfer(peers[i].ip, peers[i].port, cmd);
                    break;
                }
            }
            // Read "Transfer OK!" response from server
            char buf[MAXLINE];
            if (ssl_recv_line(srv_ssl, buf, sizeof(buf)) > 0) {
                printf("%s\n", buf);
            }
        }
        else if (strchr(cmd, '#')) {
            // LOGIN: user#port (one #, not REGISTER#)
            // Server sends LIST format response after successful login
            char tmp[MAXLINE];
            strncpy(tmp, cmd, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            char *p = strchr(tmp, '#');
            if (p) {
                *p = 0;
                strncpy(my_name, tmp, sizeof(my_name) - 1);
                my_name[sizeof(my_name) - 1] = '\0';
                my_port = atoi(p + 1);
            }
            read_list();
            // Start P2P listener thread after successful login
            if (my_port > 0) {
                pthread_t tid;
                pthread_create(&tid, NULL, p2p_listener, &my_port);
                pthread_detach(tid);
            }
        }
        else {
            // Other commands return single line
            char buf[MAXLINE];
            if (ssl_recv_line(srv_ssl, buf, sizeof(buf)) > 0) {
                printf("%s\n", buf);
            }
        }
    }

    SSL_shutdown(srv_ssl);
    SSL_free(srv_ssl);
    close(srv_fd);
    SSL_CTX_free(srv_ctx);
    EVP_cleanup();
    return 0;
}
