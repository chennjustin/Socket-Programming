#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAXLINE 4096
#define MAXUSERS 256

typedef struct {
    char name[64];
    char ip[64];
    int  port;
} Peer;

static int srv_fd = -1;           // connection to server
static int listen_fd = -1;        // local listening socket for P2P
static char my_name[64] = {0};
static Peer peers[MAXUSERS];
static int  peer_cnt = 0;
static char server_pubkey_line[MAXLINE] = {0}; // stored for later phases

// --- util ---
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap); va_end(ap);
    if (errno) fprintf(stderr, ": %s\n", strerror(errno));
    exit(1);
}
static ssize_t writen(int fd, const void *vptr, size_t n) {
    size_t left = n; const char *p = (const char*)vptr; ssize_t w;
    while (left > 0) {
        if ((w = write(fd, p, left)) <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        left -= w; p += w;
    }
    return (ssize_t)n;
}
static int send_line_crlf(int fd, const char *fmt, ...) {
    char buf[MAXLINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf)-1, fmt, ap);
    va_end(ap);

    // --- Trim any trailing whitespace/newlines ---
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == '\n' || isspace((unsigned char)buf[len-1]))) {
        buf[--len] = '\0';
    }

    // --- Send without any line ending for testing ---
    return (int)writen(fd, buf, strlen(buf));
}
static int recv_line_crlf(int fd, char *out, size_t cap) {
    size_t k = 0;
    while (k + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) {                      // peer closed
            if (k == 0) return 0;          // no data
            break;                         // return the partial line
        }
        if (n < 0) {
            if (errno == EINTR) continue;  // retry on interrupt
            return -1;
        }
        out[k++] = c;
        if (c == '\n') break;              // stop on LF (works for both LF and CRLF)
    }
    if (k == 0) return 0;
    
    // Trim optional '\r' before '\n' (handles both CRLF and LF)
    size_t end = k;
    if (end > 0 && out[end-1] == '\n') end--;
    if (end > 0 && out[end-1] == '\r') end--;
    out[end] = '\0';
    return 1;
}
static int connect_tcp(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) die("inet_pton");
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) die("connect");
    return fd;
}

// Smart P2P connection with fallback to localhost for VM/internal IPs
static int connect_tcp_p2p(const char *ip, int port) {
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    
    // Try the reported IP first
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    if (inet_pton(AF_INET, ip, &sa.sin_addr) == 1) {
        // Set short timeout for connection attempt
        struct timeval timeout;
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        
        if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) {
            // Success with reported IP
            return fd;
        }
    }
    
    // Failed with reported IP, try localhost fallback
    // (handles VM/Docker scenarios where both clients on same host)
    close(fd);
    
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) {
        // Success with localhost
        return fd;
    }
    
    // Both failed
    close(fd);
    return -1;
}
static int listen_tcp_any(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");
    int on = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) die("bind");
    if (listen(fd, 64) < 0) die("listen");
    return fd;
}
static void clear_peers(void){ peer_cnt = 0; memset(peers, 0, sizeof(peers)); }
static void add_peer(const char *name, const char *ip, int port){
    if (peer_cnt >= MAXUSERS) return;
    strncpy(peers[peer_cnt].name, name, sizeof(peers[peer_cnt].name)-1);
    strncpy(peers[peer_cnt].ip, ip, sizeof(peers[peer_cnt].ip)-1);
    peers[peer_cnt].port = port;
    peer_cnt++;
}
static Peer* find_peer(const char *name){
    for (int i=0;i<peer_cnt;i++) if (strcmp(peers[i].name,name)==0) return &peers[i];
    return NULL;
}

// --- protocol ops ---
static void parse_list_block_rest_after_balance(void) {
    char line[MAXLINE];

    // Read and print public key
    if (recv_line_crlf(srv_fd, line, sizeof(line)) <= 0) die("server closed");
    strncpy(server_pubkey_line, line, sizeof(server_pubkey_line)-1);
    printf("%s\n", line);

    // Read and print number of online users
    if (recv_line_crlf(srv_fd, line, sizeof(line)) <= 0) die("server closed");
    int n = atoi(line);
    printf("%s\n", line);

    // Read and print each user, and store in peers list
    clear_peers();
    for (int i=0;i<n;i++){
        if (recv_line_crlf(srv_fd, line, sizeof(line)) <= 0) die("server closed");
        printf("%s\n", line);
        
        // Parse and store peer info
        char line_copy[MAXLINE];
        strncpy(line_copy, line, sizeof(line_copy)-1);
        char *p1 = strchr(line_copy, '#'); if(!p1) continue; *p1++ = 0;
        char *p2 = strchr(p1, '#');   if(!p2) continue; *p2++ = 0;
        add_peer(line_copy, p1, atoi(p2));
    }
}

static void parse_list_block_all_from_socket(void){
    char line[MAXLINE];
    if (recv_line_crlf(srv_fd, line, sizeof(line)) <= 0) die("server closed");
    printf("%s\n", line); // print balance
    parse_list_block_rest_after_balance();
}

static void handle_peer_conn(int fd){
    char buf[MAXLINE]; ssize_t n=0, tot=0;
    while ((n = read(fd, buf+tot, sizeof(buf)-1-tot)) > 0) { tot += n; if (buf[tot-1]=='\n') break; }
    if (tot <= 0) { close(fd); return; }
    buf[tot] = 0;
    printf("[recv P2P] %s", buf);

    // forward to server as-is, with CRLF
    if (srv_fd >= 0) {
        char *nl = strrchr(buf, '\n'); if (nl) *nl = '\0';
        send_line_crlf(srv_fd, "%s", buf);
    } else {
        fprintf(stderr,"[!] No server connection; cannot forward.\n");
    }
    close(fd);
}

int main(int argc, char *argv[]){
    signal(SIGPIPE, SIG_IGN);
    
    // Parse command-line arguments: ./client <server_ip> <server_port>
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port>\n", argv[0]);
        exit(1);
    }
    
    const char *srv_ip = argv[1];
    int srv_port = atoi(argv[2]);
    
    // Connect to server immediately
    srv_fd = connect_tcp(srv_ip, srv_port);
    printf("Connected to the server!\n");
    printf(">");
    fflush(stdout);

    int peer_fds[FD_SETSIZE]; int peer_num = 0;

    for(;;){
        fd_set rd; FD_ZERO(&rd);
        FD_SET(STDIN_FILENO, &rd);
        int maxfd = STDIN_FILENO;
        if (srv_fd >= 0) { FD_SET(srv_fd, &rd); if (srv_fd>maxfd) maxfd=srv_fd; }
        if (listen_fd >= 0){ FD_SET(listen_fd,&rd); if(listen_fd>maxfd) maxfd=listen_fd; }
        for (int i=0;i<peer_num;i++){ FD_SET(peer_fds[i], &rd); if(peer_fds[i]>maxfd) maxfd=peer_fds[i]; }

        if (select(maxfd+1, &rd, NULL, NULL, NULL) < 0) {
            if (errno==EINTR) continue; die("select");
        }

        // stdin - handle user input
        if (FD_ISSET(STDIN_FILENO, &rd)){
            char line[MAXLINE];
            if (!fgets(line, sizeof(line), stdin)) break;
            
            char *p=line; while(isspace((unsigned char)*p)) p++;
            char *nl=strchr(p,'\n'); if(nl) *nl=0;
            
            if (!*p) {
                printf(">");
                fflush(stdout);
                continue;
            }

            // Check for REGISTER# command
            if (strncmp(p, "REGISTER#", 9) == 0) {
                printf("Awaiting server response...\n");
                send_line_crlf(srv_fd, "%s", p);
                char resp[MAXLINE];
                if (recv_line_crlf(srv_fd, resp, sizeof(resp)) > 0) {
                    printf("%s\n", resp);
                }
                printf(">");
                fflush(stdout);
            }
            // Check for P2P payment format: <payer>#<amount>#<payee>
            else if (strchr(p, '#') != NULL && strncmp(p, "REGISTER#", 9) != 0 && strchr(strchr(p, '#')+1, '#') != NULL) {
                // This is a P2P payment: AA#1000#BB
                printf("Auto renew list from tracker before transfer...\n");
                
                // Refresh list before payment
                send_line_crlf(srv_fd, "List");
                parse_list_block_all_from_socket();
                
                // Extract payee info
                char payment_copy[256];
                strncpy(payment_copy, p, sizeof(payment_copy)-1);
                char *first_hash = strchr(payment_copy, '#');
                if (first_hash) {
                    char *second_hash = strchr(first_hash+1, '#');
                    if (second_hash) {
                        *first_hash = '\0';
                        *second_hash = '\0';
                        char *payer = payment_copy;
                        char *amount = first_hash + 1;
                        char *payee = second_hash + 1;
                        
                        // Find payee in peer list
                        Peer *peer = find_peer(payee);
                        if (peer) {
                            // Send P2P message to payee (with VM/internal IP fallback)
                            int pfd = connect_tcp_p2p(peer->ip, peer->port);
                            if (pfd < 0) {
                                fprintf(stderr, "Failed to connect to peer %s at %s:%d\n", 
                                        payee, peer->ip, peer->port);
                                printf(">");
                                fflush(stdout);
                                continue;
                            }
                            char msg[MAXLINE];
                            snprintf(msg, sizeof(msg), "%s#%s#%s\n", payer, amount, payee);
                            writen(pfd, msg, strlen(msg));
                            close(pfd);
                            
                            printf("Transfer OK!\n");
                            
                            // Small delay to let server process the transaction
                            usleep(100000); // 100ms delay
                            
                            printf("auto renew list from tracker after transfer...\n");
                            
                            // Refresh list after payment
                            send_line_crlf(srv_fd, "List");
                            parse_list_block_all_from_socket();
                        } else {
                            fprintf(stderr, "Payee not found in online list\n");
                        }
                    }
                }
                printf(">");
                fflush(stdout);
            }
            // Check for login format: <name>#<port>
            else if (strchr(p, '#') != NULL && strncmp(p, "REGISTER#", 9) != 0) {
                // Extract name and port for P2P listening
                char name_copy[256];
                strncpy(name_copy, p, sizeof(name_copy)-1);
                char *hash = strchr(name_copy, '#');
                if (hash) {
                    *hash = '\0';
                    strncpy(my_name, name_copy, sizeof(my_name)-1);
                    int my_port = atoi(hash+1);
                    if (listen_fd < 0 && my_port > 0) {
                        listen_fd = listen_tcp_any(my_port);
                    }
                }
                
                printf("Awaiting server response...\n");
                send_line_crlf(srv_fd, "%s", p);
                
                // Receive login response
                char first[MAXLINE];
                if (recv_line_crlf(srv_fd, first, sizeof(first)) > 0) {
                    if (strncmp(first, "220 ", 4) == 0) {
                        printf("%s\n", first);
                    } else {
                        // Parse login success response
                        printf("%s\n", first); // balance
                        parse_list_block_rest_after_balance();
                    }
                }
                printf(">");
                fflush(stdout);
            }
            // Check for List command
            else if (strcmp(p, "List") == 0) {
                printf("Awaiting server response...\n");
                send_line_crlf(srv_fd, "List");
                parse_list_block_all_from_socket();
                printf(">");
                fflush(stdout);
            }
            // Check for Exit command
            else if (strcmp(p, "Exit") == 0) {
                send_line_crlf(srv_fd, "Exit");
                char resp[MAXLINE];
                if (recv_line_crlf(srv_fd, resp, sizeof(resp)) > 0) {
                    printf("%s\n", resp);
                }
                break;
            }
            // Unknown command - send as-is to server
            else {
                printf("Awaiting server response...\n");
                send_line_crlf(srv_fd, "%s", p);
                char resp[MAXLINE];
                if (recv_line_crlf(srv_fd, resp, sizeof(resp)) > 0) {
                    printf("%s\n", resp);
                }
                printf(">");
                fflush(stdout);
            }
        }

        // server incoming (optional messages)
        if (srv_fd >= 0 && FD_ISSET(srv_fd, &rd)){
            char line[MAXLINE];
            int r = recv_line_crlf(srv_fd, line, sizeof(line));
            if (r <= 0) { 
                printf("[server closed]\n"); 
                close(srv_fd); srv_fd=-1; 
            }
            else {
                printf("[server] %s\n", line);
            }
        }

        // new peer connection
        if (listen_fd >= 0 && FD_ISSET(listen_fd, &rd)){
            struct sockaddr_in ca; socklen_t cl=sizeof(ca);
            int cfd = accept(listen_fd, (struct sockaddr*)&ca, &cl);
            if (cfd >= 0) {
                if (peer_num < (int)(sizeof(peer_fds)/sizeof(peer_fds[0]))) {
                    peer_fds[peer_num++] = cfd;
                } else close(cfd);
            }
        }
        // handle peer sockets
        for (int i=0;i<peer_num;){
            int fd = peer_fds[i];
            if (FD_ISSET(fd, &rd)){
                handle_peer_conn(fd);
                peer_fds[i] = peer_fds[peer_num-1];
                peer_num--;
            } else i++;
        }
    }
    return 0;
}
