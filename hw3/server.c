/****************************************************
 *  TLS + Multithreaded Micropayment Server (Phase 3)
 *  - Wraps each TCP connection with TLS (OpenSSL)
 *  - Compatible with your existing protocol:
 *      REGISTER / LOGIN / LIST / EXIT / TRANSFER
 *  - Modes: -a / -d / -s
 *
 *  Build:
 *    gcc server.c -o server -lssl -lcrypto -lpthread
 *
 *  Cert (run once in same folder as server):
 *    openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt -days 365 -nodes
 *
 *  Run:
 *    ./server 8888 -a
 ****************************************************/

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <ctype.h>
 #include <pthread.h>
 #include <arpa/inet.h>
 #include <netinet/in.h>
 #include <sys/socket.h>
 
 /* OpenSSL */
 #include <openssl/ssl.h>
 #include <openssl/err.h>
 
 #define MAXLINE 4096
 #define DEFAULT_BALANCE 10000.0
 
 // ================================================
 // server modes: -a / -d / -s
 // ================================================
 typedef enum { MODE_ALL, MODE_DEBUG, MODE_SILENT } Mode;
 Mode server_mode = MODE_ALL;
 
 void logmsg(const char *msg, Mode min)
 {
     if (server_mode == MODE_SILENT) return;
     if (server_mode == MODE_DEBUG && min == MODE_ALL) return;
 
     printf("%s\n", msg);
     fflush(stdout);
 }
 
 // ================================================
 // TLS helpers
 // ================================================
 static void tls_fatal(const char *where)
 {
     fprintf(stderr, "[TLS] %s failed\n", where);
     ERR_print_errors_fp(stderr);
 }
 
 static void ssl_send_all(SSL *ssl, const char *msg)
 {
     if (!ssl || !msg) return;
     (void)SSL_write(ssl, msg, (int)strlen(msg));
 }
 
 static int ssl_recv_line(SSL *ssl, char *buf, int bufsz)
 {
     // Minimal: one SSL_read. Same behavior as your old recv().
     // For this HW protocol (short commands), it's typically enough.
     // If you want strict line framing, implement a read-until '\n' buffer.
     int n = SSL_read(ssl, buf, bufsz - 1);
     if (n <= 0) return n;
     buf[n] = '\0';
     return n;
 }
 
 static void trim(char *s)
 {
     int n = (int)strlen(s);
     while (n > 0 && (s[n-1]=='\n' || s[n-1]=='\r')) { s[n-1]=0; n--; }
 }
 
 // ================================================
 // User struct
 //   NOTE: For TLS, we store SSL* for online users
 // ================================================
 typedef struct {
     char name[64];
     char ip[64];
     int  port;
     double balance;
     int sockfd;     // underlying TCP fd
     SSL *ssl;       // TLS session (online only)
 } User;
 
 static User registered[256];
 static int reg_count = 0;
 
 static User online[256];
 static int online_count = 0;
 
 pthread_mutex_t mtx_reg = PTHREAD_MUTEX_INITIALIZER;
 pthread_mutex_t mtx_on  = PTHREAD_MUTEX_INITIALIZER;
 
 // ================================================
 // Find user helpers
 // ================================================
 static User* find_registered(const char *name)
 {
     for (int i = 0; i < reg_count; i++)
         if (strcmp(registered[i].name, name) == 0)
             return &registered[i];
     return NULL;
 }
 
 static User* find_online(const char *name)
 {
     for (int i = 0; i < online_count; i++)
         if (strcmp(online[i].name, name) == 0)
             return &online[i];
     return NULL;
 }
 
 static User* find_online_by_ssl(SSL *ssl)
 {
     for (int i = 0; i < online_count; i++)
         if (online[i].ssl == ssl)
             return &online[i];
     return NULL;
 }
 
 // ================================================
 // Send LIST
 // ================================================
 void send_list(SSL *ssl, const char *username)
{
    User *u = find_registered(username);
    if (!u) {
        SSL_write(ssl, "220 AUTH_FAIL\n", 14);
        return;
    }

    char out[MAXLINE * 4];
    int len = 0;

    // balance
    len += snprintf(out + len, sizeof(out) - len,
                    "%.0f\n", u->balance);

    // public key
    len += snprintf(out + len, sizeof(out) - len,
                    "public key\n");

    pthread_mutex_lock(&mtx_on);
    len += snprintf(out + len, sizeof(out) - len,
                    "%d\n", online_count);

    for (int i = 0; i < online_count; i++) {
        len += snprintf(out + len, sizeof(out) - len,
                        "%s#%s#%d\n",
                        online[i].name,
                        online[i].ip,
                        online[i].port);
    }
    pthread_mutex_unlock(&mtx_on);

    SSL_write(ssl, out, len);
}

 
 // ================================================
 // REGISTER
 // ================================================
 static void handle_register(SSL *ssl, const char *msg)
 {
     const char *name = msg + 9; // after "REGISTER#"
     if (!*name) { ssl_send_all(ssl, "210 FAIL\n"); return; }
 
     pthread_mutex_lock(&mtx_reg);
 
     if (find_registered(name)) {
         pthread_mutex_unlock(&mtx_reg);
         ssl_send_all(ssl, "210 FAIL\n");
         logmsg("[REGISTER] FAIL (duplicate)", MODE_ALL);
         return;
     }
 
     strcpy(registered[reg_count].name, name);
     registered[reg_count].balance = DEFAULT_BALANCE;
     reg_count++;
 
     pthread_mutex_unlock(&mtx_reg);
 
     ssl_send_all(ssl, "100 OK\n");
     logmsg("[REGISTER] OK", MODE_ALL);
 }
 
 // ================================================
 // LOGIN: "user#port"
 // ================================================
 static void handle_login(SSL *ssl, const char *msg, const char *ip)
 {
     char tmp[256];
     strncpy(tmp, msg, sizeof(tmp)-1);
     tmp[sizeof(tmp)-1] = '\0';
 
     char *p = strchr(tmp, '#');
     if (!p) { ssl_send_all(ssl, "220 AUTH_FAIL\n"); return; }
     *p = 0;
 
     char *username = tmp;
     int port = atoi(p + 1);
 
     pthread_mutex_lock(&mtx_reg);
     User *u = find_registered(username);
     pthread_mutex_unlock(&mtx_reg);
 
     if (!u) {
         ssl_send_all(ssl, "220 AUTH_FAIL\n");
         return;
     }
 
     // Put user into online list (if not already)
     pthread_mutex_lock(&mtx_on);
     User *ou = find_online(username);
     if (!ou) {
         strcpy(online[online_count].name, username);
         strcpy(online[online_count].ip, ip);
         online[online_count].port   = port;
         online[online_count].ssl    = ssl;
         online[online_count].sockfd = SSL_get_fd(ssl);
         online_count++;
     } else {
         // already online: refresh session info
         strcpy(ou->ip, ip);
         ou->port   = port;
         ou->ssl    = ssl;
         ou->sockfd = SSL_get_fd(ssl);
     }
     pthread_mutex_unlock(&mtx_on);
 
     // login success → send List
     send_list(ssl, username);
     logmsg("[LOGIN] OK", MODE_ALL);
 }
 
 // ================================================
 // EXIT: remove from online list + close TLS
 // ================================================
 static void handle_exit(SSL *ssl)
 {
     if (!ssl) return;
 
     pthread_mutex_lock(&mtx_on);
     for (int i = 0; i < online_count; i++) {
         if (online[i].ssl == ssl) {
             for (int j = i; j < online_count - 1; j++)
                 online[j] = online[j + 1];
             online_count--;
             break;
         }
     }
     pthread_mutex_unlock(&mtx_on);
 
     ssl_send_all(ssl, "Bye\n");
 
     int fd = SSL_get_fd(ssl);
     SSL_shutdown(ssl);
     SSL_free(ssl);
     if (fd >= 0) close(fd);
 }
 
 // ================================================
 // TRANSFER: "A#amt#B"
 // (Triggered when receiver forwards P2P message to server)
 // ================================================
 static void handle_transfer(const char *msg)
 {
     char tmp[256];
     strncpy(tmp, msg, sizeof(tmp)-1);
     tmp[sizeof(tmp)-1] = '\0';
 
     char *h1 = strchr(tmp, '#');
     char *h2 = h1 ? strchr(h1 + 1, '#') : NULL;
 
     if (!h1 || !h2) return;
     *h1 = 0;
     *h2 = 0;
 
     char *sender   = tmp;
     char *amount_s = h1 + 1;
     char *receiver = h2 + 1;
 
     double amount = atof(amount_s);
     if (amount <= 0) return;
 
     pthread_mutex_lock(&mtx_reg);
 
     User *s = find_registered(sender);
     User *r = find_registered(receiver);
     if (!s || !r || s->balance < amount) {
         pthread_mutex_unlock(&mtx_reg);
         return;
     }
 
     s->balance -= amount;
     r->balance += amount;
 
     pthread_mutex_unlock(&mtx_reg);
 
     // notify sender via its TLS session (if online)
     pthread_mutex_lock(&mtx_on);
     User *os = find_online(sender);
     if (os && os->ssl) ssl_send_all(os->ssl, "Transfer OK!\n");
     pthread_mutex_unlock(&mtx_on);
 
     logmsg("[TRANSFER] done", MODE_ALL);
 }
 
 // ================================================
 // Client worker thread: SSL_read / SSL_write loop
 // ================================================
 static void* worker(void *arg)
 {
     SSL *ssl = (SSL*)arg;
     if (!ssl) return NULL;
 
     // peer ip
     int fd = SSL_get_fd(ssl);
     struct sockaddr_in addr;
     socklen_t len = sizeof(addr);
     memset(&addr, 0, sizeof(addr));
     if (getpeername(fd, (struct sockaddr*)&addr, &len) != 0) {
         // fallback
         addr.sin_addr.s_addr = 0;
     }
 
     char ip[64];
     const char *ip_s = inet_ntoa(addr.sin_addr);
     if (ip_s) strncpy(ip, ip_s, sizeof(ip)-1);
     else strcpy(ip, "0.0.0.0");
     ip[sizeof(ip)-1] = '\0';
 
     char buf[MAXLINE];
 
     while (1) {
         memset(buf, 0, sizeof(buf));
         int n = ssl_recv_line(ssl, buf, sizeof(buf));
         if (n <= 0) {
             // client closed or TLS error
             handle_exit(ssl);
             return NULL;
         }
 
         trim(buf);
 
         if (server_mode == MODE_ALL) {
             printf("[recv] %s\n", buf);
             fflush(stdout);
         }
 
         if (strncmp(buf, "REGISTER#", 9) == 0) {
             handle_register(ssl, buf);
         }
         else if (strcmp(buf, "List") == 0) {
             pthread_mutex_lock(&mtx_on);
             User *u = find_online_by_ssl(ssl);
             pthread_mutex_unlock(&mtx_on);
             if (u) send_list(ssl, u->name);
             else  ssl_send_all(ssl, "220 AUTH_FAIL\n");
         }
         else if (strcmp(buf, "Exit") == 0) {
             handle_exit(ssl);
             return NULL;
         }
         else if (strchr(buf, '#') && strchr(strchr(buf,'#') + 1, '#')) {
             // TRANSFER: A#amt#B
             handle_transfer(buf);
             // (no direct reply required by your existing protocol)
         }
         else if (strchr(buf, '#')) {
             // LOGIN: user#port
             handle_login(ssl, buf, ip);
         }
         else {
             ssl_send_all(ssl, "Unknown Command\n");
         }
     }
     return NULL;
 }
 
 // ================================================
 // MAIN
 // ================================================
 int main(int argc, char *argv[])
 {
     if (argc != 3) {
         printf("Usage: %s <port> < -a | -d | -s >\n", argv[0]);
         exit(1);
     }
 
     int port = atoi(argv[1]);
     if (port < 1024 || port > 65535) {
         printf("Invalid port\n");
         exit(1);
     }
 
     if      (strcmp(argv[2], "-a") == 0) server_mode = MODE_ALL;
     else if (strcmp(argv[2], "-d") == 0) server_mode = MODE_DEBUG;
     else if (strcmp(argv[2], "-s") == 0) server_mode = MODE_SILENT;
     else {
         printf("Invalid mode\n");
         exit(1);
     }
 
     // ---- OpenSSL init ----
     SSL_library_init();
     SSL_load_error_strings();
     OpenSSL_add_all_algorithms();
 
     SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
     if (!ctx) {
         tls_fatal("SSL_CTX_new");
         exit(1);
     }
 
     // Load cert/key from current folder
     if (SSL_CTX_use_certificate_file(ctx, "cert/server.crt", SSL_FILETYPE_PEM) <= 0) {
         tls_fatal("use_certificate_file(server.crt)");
         SSL_CTX_free(ctx);
         exit(1);
     }
     if (SSL_CTX_use_PrivateKey_file(ctx, "cert/server.key", SSL_FILETYPE_PEM) <= 0) {
         tls_fatal("use_privatekey_file(server.key)");
         SSL_CTX_free(ctx);
         exit(1);
     }
     if (!SSL_CTX_check_private_key(ctx)) {
         fprintf(stderr, "[TLS] Private key does not match the certificate public key\n");
         SSL_CTX_free(ctx);
         exit(1);
     }
 
     // ---- TCP listen socket ----
     int listenfd = socket(AF_INET, SOCK_STREAM, 0);
     if (listenfd < 0) {
         perror("socket");
         SSL_CTX_free(ctx);
         exit(1);
     }
 
     int yes = 1;
     setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
 
     struct sockaddr_in serv;
     memset(&serv, 0, sizeof(serv));
     serv.sin_family = AF_INET;
     serv.sin_addr.s_addr = INADDR_ANY;
     serv.sin_port = htons(port);
 
     if (bind(listenfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
         perror("bind");
         close(listenfd);
         SSL_CTX_free(ctx);
         exit(1);
     }
 
     if (listen(listenfd, 64) < 0) {
         perror("listen");
         close(listenfd);
         SSL_CTX_free(ctx);
         exit(1);
     }
 
     printf("Server running at port %d mode=%s (TLS enabled)\n",
         port,
         server_mode==MODE_ALL?"-a":server_mode==MODE_DEBUG?"-d":"-s"
     );
     fflush(stdout);
 
     // ---- accept loop ----
     while (1) {
         int client_fd = accept(listenfd, NULL, NULL);
         if (client_fd < 0) continue;
 
         // Wrap with TLS
         SSL *ssl = SSL_new(ctx);
         if (!ssl) {
             close(client_fd);
             continue;
         }
         SSL_set_fd(ssl, client_fd);
 
         if (SSL_accept(ssl) <= 0) {
             tls_fatal("SSL_accept");
             SSL_free(ssl);
             close(client_fd);
             continue;
         }
 
         pthread_t tid;
         pthread_create(&tid, NULL, worker, ssl);
         pthread_detach(tid);
     }
 
     // normally unreachable
     close(listenfd);
     SSL_CTX_free(ctx);
     return 0;
 }
 