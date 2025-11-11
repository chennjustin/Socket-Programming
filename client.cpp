#include <iostream>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 4096
using namespace std;

// 傳送完整封包，確保所有資料都送出
bool send_all(int sock, const string &data) {
    size_t total = 0;
    while (total < data.size()) {
        ssize_t sent = send(sock, data.c_str() + total, data.size() - total, 0);
        if (sent <= 0) return false;
        total += static_cast<size_t>(sent);
    }
    return true;
}

// 接收伺服器回覆
string receive_response(int sock) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0) return "";
    buffer[bytes] = '\0';
    return string(buffer);
}

// 去除換行空白
string trim(const string &str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

// 主程式
int main(int argc, char *argv[]) {
    if (argc != 3) {
        cerr << "使用方式: ./client <Server_IP> <Server_Port>" << endl;
        return 1;
    }

    string server_ip = argv[1];
    int server_port = stoi(argv[2]);

    // === 建立 TCP socket 並連線到 server ===
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        cerr << "無法建立 socket。\n";
        return 1;
    }

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip.c_str(), &serv_addr.sin_addr);

    if (connect(sock, (sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        cerr << "無法連接伺服器。\n";
        close(sock);
        return 1;
    }

    cout << "Connected to the server!\n";

    // === 紀錄已註冊帳號，用來檢查重複註冊 ===
    string registered_user = "";

    // === 主互動迴圈 ===
    while (true) {
        cout << "\n=== 選單 ===\n";
        cout << "1. 註冊 (REGISTER)\n";
        cout << "2. 登入 (LOGIN)\n";
        cout << "3. 查詢 (LIST)\n";
        cout << "4. 離線 (EXIT)\n";
        cout << "請選擇功能 [1-4]: ";

        int choice;
        if (!(cin >> choice)) { // 非整數輸入
            cout << "指令錯誤，請輸入數字 1~4。\n";
            cin.clear();
            cin.ignore(1024, '\n');
            continue;
        }
        cin.ignore();

        string message;
        if (choice == 1) {
            // === 註冊 ===
            string username;
            cout << "輸入註冊名稱: ";
            getline(cin, username);
            username = trim(username);

            if (username.empty()) {
                cout << "使用者名稱不可為空。\n";
                continue;
            }
            if (username == registered_user) {
                cout << "該名稱已註冊，請換一個名稱。\n";
                continue;
            }

            message = "REGISTER#" + username;

            // 傳送註冊請求
            if (!send_all(sock, message)) {
                cerr << "傳送失敗，斷線。\n";
                break;
            }

            // 等待伺服器回覆
            string response = receive_response(sock);
            if (response.empty()) {
                cout << "伺服器無回應或連線關閉。\n";
                break;
            }

            cout << "\n--- Server Response ---\n" << response << "------------------------\n";

            // 解析伺服器回覆
            if (response.find("100 OK") != string::npos) {
                cout << "註冊成功！\n";
                registered_user = username; // 紀錄成功註冊的名稱
            } else if (response.find("210 FAIL") != string::npos) {
                cout << "註冊失敗：名稱重複或無效。\n";
            } else {
                cout << "未知的伺服器回覆。\n";
            }

        } else if (choice == 2) {
            // === 登入 ===
            string username;
            cout << "輸入使用者名稱: ";
            getline(cin, username);
            username = trim(username);

            if (username.empty()) {
                cout << "名稱不可為空。\n";
                continue;
            }

            // 如果此名稱不是剛剛註冊過的，則報錯
            if (username != registered_user) {
                cout << "名稱尚未註冊，請先註冊再登入。\n";
                continue;
            }

            // 名稱存在，繼續輸入 Port
            string port;
            cout << "輸入 Client Port Number (1024~65535): ";
            getline(cin, port);
            port = trim(port);

            int port_num = 0;
            try {
                port_num = stoi(port);
            } catch (...) {
                cout << "Port 必須是數字。\n";
                continue;
            }

            if (port_num < 1024 || port_num > 65535) {
                cout << "Port 必須介於 1024 與 65535 之間。\n";
                continue;
            }

            message = username + "#" + port;

            // 傳送登入請求
            if (!send_all(sock, message)) {
                cerr << "傳送失敗，斷線。\n";
                break;
            }

            string response = receive_response(sock);
            if (response.empty()) {
                cout << "伺服器無回應或連線關閉。\n";
                break;
            }

            cout << "\n--- Server Response ---\n" << response << "------------------------\n";

            if (response.find("220 AUTH_FAIL") != string::npos) {
                cout << "登入失敗：帳號未註冊或驗證錯誤。\n";
            } else {
                cout << "登入成功！顯示上線清單如下。\n";
            }

        } else if (choice == 3) {
            // === 查詢清單 ===
            message = "List";
            if (!send_all(sock, message)) {
                cerr << "傳送失敗，斷線。\n";
                break;
            }

            string response = receive_response(sock);
            if (response.empty()) {
                cout << "伺服器無回應或連線關閉。\n";
                break;
            }

            cout << "\n--- Server Response ---\n" << response << "------------------------\n";

        } else if (choice == 4) {
            // === 離線 ===
            message = "Exit";
            if (!send_all(sock, message)) {
                cerr << "傳送失敗，斷線。\n";
                break;
            }

            string response = receive_response(sock);
            cout << "\n--- Server Response ---\n" << response << "------------------------\n";
            cout << "已離線。\n";
            break;

        } else {
            // === 錯誤指令 ===
            cout << "指令錯誤，請輸入 1~4。\n";
        }
    }

    close(sock);
    return 0;
}
