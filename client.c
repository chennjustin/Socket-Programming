#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/* === OpenSSL === */
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
static SSL_CTX *ssl_ctx = NULL;
static SSL *ssl = NULL;

static int listen_fd = -1;
static char my_name[64] = {0};
static Peer peers[MAXUSERS];
static int  peer_cnt = 0;

/* ===== Utility ===== */
static void die(const char *msg){
    perror(msg);
    exit(1);
}

static void ssl_die(void){
    ERR_print_errors_fp(stderr);
    exit(1);
}

static ssize_t ssl_writen(const void *buf, size_t n){
    size_t sent = 0;
    while (sent < n) {
        int r = SSL_write(ssl, (char*)buf + sent, n - sent);
        if (r <= 0) return -1;
        sent += r;
    }
    return sent;
}

static int ssl_send_line(const char *fmt, ...){
    char buf[MAXLINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return ssl_writen(buf, strlen(buf));
}

static int ssl_recv_line(char *out, size_t cap){
    size_t i = 0;
    while (i + 1 < cap) {
        char c;
        int r = SSL_read(ssl, &c, 1);
        if (r <= 0) return -1;
        if (c == '\n') break;
        out[i++] = c;
    }
    out[i] = '\0';
    return 1;
}

static int handle_list_response(void){
    char resp[MAXLINE];
    
    // Read balance
    if (ssl_recv_line(resp, sizeof(resp)) <= 0) {
        printf("Server closed\n");
        return -1;
    }
    printf("%s\n", resp);
    
    // Read public key
    if (ssl_recv_line(resp, sizeof(resp)) <= 0) {
        printf("Server closed\n");
        return -1;
    }
    printf("%s\n", resp);
    
    // Read count
    if (ssl_recv_line(resp, sizeof(resp)) <= 0) {
        printf("Server closed\n");
        return -1;
    }
    printf("%s\n", resp);
    int count = atoi(resp);
    
    // Read N user lines
    for (int i = 0; i < count; i++) {
        if (ssl_recv_line(resp, sizeof(resp)) <= 0) {
            printf("Server closed\n");
            return -1;
        }
        printf("%s\n", resp);
    }
    
    return 0;
}

/* ===== TCP connect ===== */
static int connect_tcp(const char *ip, int port){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, ip, &sa.sin_addr);

    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0)
        die("connect");

    return fd;
}

/* ===== Main ===== */
int main(int argc, char *argv[]){
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port>\n", argv[0]);
        exit(1);
    }

    const char *sip = argv[1];
    int sport = atoi(argv[2]);

    /* === OpenSSL init === */
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx) ssl_die();

    /* === TCP connect === */
    srv_fd = connect_tcp(sip, sport);

    /* === TLS handshake === */
    ssl = SSL_new(ssl_ctx);
    SSL_set_fd(ssl, srv_fd);

    if (SSL_connect(ssl) <= 0)
        ssl_die();

    printf("Connected to server (TLS enabled)\n");

    /* ===== Simple interactive loop (示範) ===== */
    char line[MAXLINE];
    char resp[MAXLINE];

    while (1) {
        printf(">");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = 0;

        ssl_send_line("%s\n", line);

        // Handle different response types
        // LOGIN (user#port) and List both return LIST format response
        // REGISTER#username returns single line, so exclude it
        if (strcmp(line, "List") == 0 || 
            (strncmp(line, "REGISTER#", 9) != 0 && strchr(line, '#') && !strchr(strchr(line, '#') + 1, '#'))) {
            // LIST response: balance, public key, count, then N user lines
            if (handle_list_response() < 0) break;
        } else {
            // Single line response
            if (ssl_recv_line(resp, sizeof(resp)) <= 0) {
                printf("Server closed\n");
                break;
            }
            printf("%s\n", resp);
        }

        if (strcmp(line, "Exit") == 0) break;
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(srv_fd);
    SSL_CTX_free(ssl_ctx);
    EVP_cleanup();
    return 0;
}
