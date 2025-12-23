 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <ctype.h>
 #include <pthread.h>
 #include <arpa/inet.h>
 #include <netinet/in.h>
 #include <sys/socket.h>
 
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
 // User struct
 // ================================================
 typedef struct {
     char name[64];
     char ip[64];
     int  port;
     double balance;
     int sockfd;
 } User;
 
 static User registered[256];
 static int reg_count = 0;
 
 static User online[256];
 static int online_count = 0;
 
 pthread_mutex_t mtx_reg = PTHREAD_MUTEX_INITIALIZER;
 pthread_mutex_t mtx_on  = PTHREAD_MUTEX_INITIALIZER;
 
 // ================================================
 // Utility
 // ================================================
 void send_all(int fd, const char *msg)
 {
     write(fd, msg, strlen(msg));
 }
 
 void trim(char *s)
 {
     int n = strlen(s);
     while (n > 0 && (s[n-1]=='\n'||s[n-1]=='\r')) { s[n-1]=0; n--; }
 }
 
 // ================================================
 // Find user helpers
 // ================================================
 User* find_registered(const char *name)
 {
     for (int i=0; i<reg_count; i++)
         if (strcmp(registered[i].name, name)==0)
             return &registered[i];
     return NULL;
 }
 
 User* find_online_by_fd(int fd)
 {
     for (int i=0; i<online_count; i++)
         if (online[i].sockfd == fd)
             return &online[i];
     return NULL;
 }
 
 User* find_online(const char *name)
 {
     for (int i=0; i<online_count; i++)
         if (strcmp(online[i].name, name)==0)
             return &online[i];
     return NULL;
 }
 
 // ================================================
 // Send LIST
 // ================================================
 void send_list(int fd, const char *username)
 {
     User *u = find_registered(username);
     if (!u) { send_all(fd, "220 AUTH_FAIL\n"); return; }
 
     char buf[MAXLINE];
 
     // balance
     snprintf(buf, sizeof(buf), "%.0f\n", u->balance);
     send_all(fd, buf);
 
     // public key (dummy)
     send_all(fd, "public key\n");
 
     // number of online users
     pthread_mutex_lock(&mtx_on);
     snprintf(buf, sizeof(buf), "%d\n", online_count);
     send_all(fd, buf);
 
     for (int i=0; i<online_count; i++) {
         snprintf(buf, sizeof(buf),
                  "%s#%s#%d\n",
                  online[i].name,
                  online[i].ip,
                  online[i].port);
         send_all(fd, buf);
     }
     pthread_mutex_unlock(&mtx_on);
 }
 
 // ================================================
 // REGISTER
 // ================================================
 void handle_register(int fd, const char *msg)
 {
     const char *name = msg + 9;
     if (!*name) { send_all(fd, "210 FAIL\n"); return; }
 
     pthread_mutex_lock(&mtx_reg);
 
     if (find_registered(name)) {
         pthread_mutex_unlock(&mtx_reg);
         send_all(fd, "210 FAIL\n");
         logmsg("[REGISTER] FAIL (duplicate)", MODE_ALL);
         return;
     }
 
     strcpy(registered[reg_count].name, name);
     registered[reg_count].balance = DEFAULT_BALANCE;
     reg_count++;
 
     pthread_mutex_unlock(&mtx_reg);
 
     send_all(fd, "100 OK\n");
     logmsg("[REGISTER] OK", MODE_ALL);
 }
 
 // ================================================
 // LOGIN
 // user#port
 // ================================================
 void handle_login(int fd, const char *msg, const char *ip)
 {
     char tmp[256];
     strcpy(tmp, msg);
 
     char *p = strchr(tmp, '#');
     if (!p) { send_all(fd, "220 AUTH_FAIL\n"); return; }
     *p = 0;
 
     char *username = tmp;
     int port = atoi(p+1);
 
     pthread_mutex_lock(&mtx_reg);
     User *u = find_registered(username);
     pthread_mutex_unlock(&mtx_reg);
 
     if (!u) {
         send_all(fd, "220 AUTH_FAIL\n");
         return;
     }
 
     pthread_mutex_lock(&mtx_on);
     User *ou = find_online(username);
     if (!ou) {
         strcpy(online[online_count].name, username);
         strcpy(online[online_count].ip,   ip);
         online[online_count].port = port;
         online[online_count].sockfd = fd;
         online_count++;
     }
     pthread_mutex_unlock(&mtx_on);
 
     // login success → send List
     send_list(fd, username);
 
     logmsg("[LOGIN] OK", MODE_ALL);
 }
 
 // ================================================
 // EXIT
 // ================================================
 void handle_exit(int fd)
 {
     pthread_mutex_lock(&mtx_on);
 
     for (int i=0; i<online_count; i++) {
         if (online[i].sockfd == fd) {
             // remove
             for (int j=i; j<online_count-1; j++)
                 online[j] = online[j+1];
             online_count--;
             break;
         }
     }
 
     pthread_mutex_unlock(&mtx_on);
 
     send_all(fd, "Bye\n");
     close(fd);
 }
 
 // ================================================
 // TRANSFER: A#amt#B
 // ================================================
 void handle_transfer(const char *msg)
 {
     char tmp[256];
     strcpy(tmp, msg);
 
     char *h1 = strchr(tmp, '#');
     char *h2 = h1 ? strchr(h1+1, '#') : NULL;
 
     if (!h1 || !h2) return;
     *h1 = 0;
     *h2 = 0;
 
     char *sender = tmp;
     char *amount_s = h1+1;
     char *receiver = h2+1;
 
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
 
     // notify sender via its online socket
     pthread_mutex_lock(&mtx_on);
     User *os = find_online(sender);
     if (os) send_all(os->sockfd, "Transfer OK!\n");
     pthread_mutex_unlock(&mtx_on);
 
     logmsg("[TRANSFER] done", MODE_ALL);
 }
 
 // ================================================
 // Client thread
 // ================================================
 void* worker(void *arg)
 {
     int fd = *(int*)arg;
     free(arg);
 
     struct sockaddr_in addr;
     socklen_t len = sizeof(addr);
     getpeername(fd, (struct sockaddr*)&addr, &len);
 
     char ip[64];
     strcpy(ip, inet_ntoa(addr.sin_addr));
 
     char buf[MAXLINE];
 
     while (1) {
         memset(buf, 0, sizeof(buf));
         int n = recv(fd, buf, sizeof(buf)-1, 0);
         if (n <= 0) {
             handle_exit(fd);
             return NULL;
         }
 
         trim(buf);
 
         // print debug
         if (server_mode == MODE_ALL)
         {
             printf("[recv] %s\n", buf);
             fflush(stdout);
         }
 
         if (strncmp(buf, "REGISTER#", 9) == 0) {
             handle_register(fd, buf);
         }
         else if (strcmp(buf, "List") == 0) {
             User *u = find_online_by_fd(fd);
             if (u) send_list(fd, u->name);
         }
         else if (strcmp(buf, "Exit") == 0) {
             handle_exit(fd);
             return NULL;
         }
         else if (strchr(buf, '#') && strchr(strchr(buf,'#')+1, '#')) {
             handle_transfer(buf);
         }
         else if (strchr(buf, '#')) {
             handle_login(fd, buf, ip);
         }
         else {
             send_all(fd, "Unknown Command\n");
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
 
     int listenfd = socket(AF_INET, SOCK_STREAM, 0);
 
     int yes = 1;
     setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
 
     struct sockaddr_in serv;
     serv.sin_family = AF_INET;
     serv.sin_addr.s_addr = INADDR_ANY;
     serv.sin_port = htons(port);
 
     bind(listenfd, (struct sockaddr*)&serv, sizeof(serv));
     listen(listenfd, 64);
 
     printf("Server running at port %d mode=%s\n",
         port,
         server_mode==MODE_ALL?"-a":server_mode==MODE_DEBUG?"-d":"-s"
     );
 
     while (1) {
         int *fd = malloc(sizeof(int));
         *fd = accept(listenfd, NULL, NULL);
         pthread_t tid;
         pthread_create(&tid, NULL, worker, fd);
         pthread_detach(tid);
     }
 
     return 0;
 } 