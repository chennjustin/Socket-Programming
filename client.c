#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BUF_SIZE 1024

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <ServerIP> <Port>\n", argv[0]);
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket error");
        return 1;
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(atoi(argv[2]));
    inet_pton(AF_INET, argv[1], &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        perror("connect error");
        close(sock);
        return 1;
    }

    printf("Connected to server!\n");

    char message[BUF_SIZE];
    while (1) {
        printf("Input message (q to quit): ");
        fgets(message, BUF_SIZE, stdin);
        if (!strcmp(message, "q\n")) break;
        write(sock, message, strlen(message));
    }

    close(sock);
    printf("Disconnected.\n");
    return 0;
}
