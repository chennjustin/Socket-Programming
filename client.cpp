#include <iostream>
#include <string>
#include <sstream>
#include <unistd.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 4096

using namespace std;

// === 功能函式宣告 ===
void register_user(int sock);
void login_user(int sock);
void get_list(int sock);
void exit_program(int sock);

// === 主程式 ===
int main() {
    string server_ip;
    int server_port;

    cout << "輸入伺服器 IP: ";
    cin >> server_ip;
    cout << "輸入伺服器 Port: ";
    cin >> server_port;

    // 建立 TCP socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        cerr << "無法建立 socket。" << endl;
        return 1;
    }

    // 設定伺服器位址
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip.c_str(), &serv_addr.sin_addr);

    // 連線伺服器
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        cerr << "無法連接到伺服器。" << endl;
        close(sock);
        return 1;
    }

    cout << "成功連線到伺服器 " << server_ip << ":" << server_port << endl;

    while (true) {
        cout << "\n=== Menu ===\n";
        cout << "1. 註冊 (REGISTER)\n";
        cout << "2. 登入 (LOGIN)\n";
        cout << "3. 查詢清單 (LIST)\n";
        cout << "4. 離線 (EXIT)\n";
        cout << "請輸入選項 [1-4]: ";

        int choice;
        cin >> choice;
        cin.ignore();

        if (choice == 1) register_user(sock);
        else if (choice == 2) login_user(sock);
        else if (choice == 3) get_list(sock);
        else if (choice == 4) { exit_program(sock); break; }
        else cout << "選項錯誤，請重新輸入。\n";
    }

    close(sock);
    return 0;
}

// === 功能實作 ===

// 註冊 REGISTER#username
void register_user(int sock) {
    string username;
    cout << "輸入註冊使用者名稱: ";
    cin >> username;

    string message = "REGISTER#" + username;
    send(sock, message.c_str(), message.length(), 0);

    char buffer[BUFFER_SIZE] = {0};
    int bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = 0;
        cout << "伺服器回覆: " << buffer << endl;
    } else {
        cout << "伺服器無回應。\n";
    }
}

// 登入 username#port
void login_user(int sock) {
    string username, port;
    cout << "輸入使用者名稱: ";
    cin >> username;
    cout << "輸入 Port Number: ";
    cin >> port;

    string message = username + "#" + port;
    send(sock, message.c_str(), message.length(), 0);

    char buffer[BUFFER_SIZE] = {0};
    int bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = 0;
        cout << "\n--- Server 回覆 ---\n" << buffer << "---------------------\n";
    } else {
        cout << "伺服器無回應。\n";
    }
}

// 查詢 List
void get_list(int sock) {
    string message = "List";
    send(sock, message.c_str(), message.length(), 0);

    char buffer[BUFFER_SIZE] = {0};
    int bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = 0;
        cout << "\n--- 上線清單 ---\n" << buffer << "-----------------\n";
    } else {
        cout << "伺服器無回應。\n";
    }
}

// 離線 Exit
void exit_program(int sock) {
    string message = "Exit";
    send(sock, message.c_str(), message.length(), 0);

    char buffer[BUFFER_SIZE] = {0};
    int bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = 0;
        cout << "伺服器回覆: " << buffer << endl;
    } else {
        cout << "伺服器無回應。\n";
    }

    cout << "離線中...\n";
}
