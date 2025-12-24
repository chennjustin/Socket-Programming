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
#include <signal.h>
#include <errno.h>

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
static pthread_mutex_t srv_ssl_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static volatile sig_atomic_t keep_running = 1;
static void signal_handler(int sig) {
    (void)sig;
    keep_running = 0;
}

static int ssl_send(const char *fmt, ...) {
    char buf[MAXLINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&srv_ssl_mutex);
    int ret = SSL_write(srv_ssl, buf, strlen(buf));
    pthread_mutex_unlock(&srv_ssl_mutex);
    return ret;
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

    // Protect entire LIST reading process
    pthread_mutex_lock(&srv_ssl_mutex);
    
    // Read balance
    size_t i = 0;
    while (i+1 < sizeof(line)) {
        char c;
        int r = SSL_read(srv_ssl, &c, 1);
        if (r <= 0) {
            pthread_mutex_unlock(&srv_ssl_mutex);
            return -1;
        }
        if (c == '\n') break;
        line[i++] = c;
    }
    line[i] = 0;
    printf("%s\n", line);           // balance

    // Read public key
    i = 0;
    while (i+1 < sizeof(line)) {
        char c;
        int r = SSL_read(srv_ssl, &c, 1);
        if (r <= 0) {
            pthread_mutex_unlock(&srv_ssl_mutex);
            return -1;
        }
        if (c == '\n') break;
        line[i++] = c;
    }
    line[i] = 0;
    printf("%s\n", line);           // public key

    // Read count
    i = 0;
    while (i+1 < sizeof(line)) {
        char c;
        int r = SSL_read(srv_ssl, &c, 1);
        if (r <= 0) {
            pthread_mutex_unlock(&srv_ssl_mutex);
            return -1;
        }
        if (c == '\n') break;
        line[i++] = c;
    }
    line[i] = 0;
    int n = atoi(line);
    printf("%d\n", n);

    // Read N user lines
    for (int j = 0; j < n; j++) {
        i = 0;
        while (i+1 < sizeof(line)) {
            char c;
            int r = SSL_read(srv_ssl, &c, 1);
            if (r <= 0) {
                pthread_mutex_unlock(&srv_ssl_mutex);
                return -1;
            }
            if (c == '\n') break;
            line[i++] = c;
        }
        line[i] = 0;
        printf("%s\n", line);
        sscanf(line, "%[^#]#%[^#]#%d",
               peers[peer_cnt].name,
               peers[peer_cnt].ip,
               &peers[peer_cnt].port);
        peer_cnt++;
    }
    
    pthread_mutex_unlock(&srv_ssl_mutex);
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
            
            printf("[P2P] Received transfer: %s\n", buf);
            fflush(stdout);
            
            // Forward to server (use mutex to protect shared srv_ssl)
            if (srv_ssl) {
                char msg[MAXLINE];
                int len = strlen(buf);
                if (len < sizeof(msg) - 2) {
                    snprintf(msg, sizeof(msg), "%s\n", buf);
                    pthread_mutex_lock(&srv_ssl_mutex);
                    int written = SSL_write(srv_ssl, msg, strlen(msg));
                    pthread_mutex_unlock(&srv_ssl_mutex);
                    if (written > 0) {
                        printf("[P2P] Forwarded to server\n");
                        fflush(stdout);
                    } else {
                        fprintf(stderr, "[P2P] Failed to forward to server\n");
                    }
                }
            } else {
                fprintf(stderr, "[P2P] No server connection available\n");
            }
        } else if (n < 0) {
            // SSL_read error, but don't exit - just close this connection
            fprintf(stderr, "[P2P] SSL_read error\n");
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

    /* === signal handling === */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* === interactive === */
    char cmd[MAXLINE];
    while (keep_running) {
        printf(">");
        fflush(stdout);
        if (!fgets(cmd, sizeof(cmd), stdin)) {
            if (feof(stdin)) {
                printf("\nEOF, exiting...\n");
                break;
            }
            if (ferror(stdin) && errno != EINTR) {
                perror("fgets");
                break;
            }
            continue;
        }
        cmd[strcspn(cmd,"\n")] = 0;

        // Check command type and handle accordingly
        // TRANSFER commands should NOT be sent to server directly
        // They are sent via P2P to receiver, who forwards to server
        if (strchr(cmd, '#') && strchr(strchr(cmd, '#') + 1, '#')) {
            // TRANSFER: A#amt#B (two #)
            // Send via P2P to receiver, receiver forwards to server
            char to[64];
            sscanf(cmd, "%*[^#]#%*[^#]#%s", to);
            int found = 0;
            for (int i = 0; i < peer_cnt; i++) {
                if (strcmp(peers[i].name, to) == 0) {
                    p2p_transfer(peers[i].ip, peers[i].port, cmd);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                printf("Payee not found in online list\n");
                continue;
            }
            // Read "Transfer OK!" response from server
            // Server sends this after receiver forwards the TRANSFER message
            pthread_mutex_lock(&srv_ssl_mutex);
            char buf[MAXLINE];
            if (ssl_recv_line(srv_ssl, buf, sizeof(buf)) > 0) {
                // Only print if it's "Transfer OK!"
                if (strncmp(buf, "Transfer OK", 11) == 0) {
                    printf("%s\n", buf);
                } else {
                    // Unexpected response - this shouldn't happen
                    // But if it does, print it anyway
                    printf("%s\n", buf);
                }
            }
            pthread_mutex_unlock(&srv_ssl_mutex);
        }
        else {
            // All other commands are sent to server
            ssl_send("%s\n", cmd);

            if (strcmp(cmd, "List") == 0) {
                // List command returns LIST format (multi-line)
                read_list();
            }
            else if (strcmp(cmd, "Exit") == 0) {
                // Exit command returns single line "Bye\n"
                pthread_mutex_lock(&srv_ssl_mutex);
                char buf[MAXLINE];
                if (ssl_recv_line(srv_ssl, buf, sizeof(buf)) > 0) {
                    printf("%s\n", buf);
                }
                pthread_mutex_unlock(&srv_ssl_mutex);
                break;
            }
            else if (strncmp(cmd, "REGISTER#", 9) == 0) {
                // REGISTER returns single line response
                pthread_mutex_lock(&srv_ssl_mutex);
                char buf[MAXLINE];
                if (ssl_recv_line(srv_ssl, buf, sizeof(buf)) > 0) {
                    printf("%s\n", buf);
                }
                pthread_mutex_unlock(&srv_ssl_mutex);
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
                    static int listener_started = 0;
                    if (!listener_started) {
                        pthread_t tid;
                        if (pthread_create(&tid, NULL, p2p_listener, &my_port) == 0) {
                            pthread_detach(tid);
                            listener_started = 1;
                            printf("[P2P] Listener started on port %d\n", my_port);
                            fflush(stdout);
                        } else {
                            fprintf(stderr, "[P2P] Failed to start listener thread\n");
                        }
                    }
                }
            }
            else {
                // Other commands return single line
                pthread_mutex_lock(&srv_ssl_mutex);
                char buf[MAXLINE];
                if (ssl_recv_line(srv_ssl, buf, sizeof(buf)) > 0) {
                    printf("%s\n", buf);
                }
                pthread_mutex_unlock(&srv_ssl_mutex);
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
