#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <sstream>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace std;

#define BUF 4096

// ------------------------ 全域狀態 ------------------------
int server_sock = -1;          // 與助教 server 的 TCP 連線
string login_user;             // 目前登入的使用者名稱
int listen_port = 0;           // 自己宣告的 P2P 監聽埠
bool logged_in = false;
bool listener_started = false; // 確保監聽只啟動一次
mutex io_mtx;                  // 印出與 server_sock 保護

// ------------------------ 工具函式 ------------------------
bool send_all(int fd, const string &data) {
    size_t sent_total = 0;
    while (sent_total < data.size()) {
        ssize_t n = send(fd, data.c_str() + sent_total, data.size() - sent_total, 0);
        if (n <= 0) return false;
        sent_total += (size_t)n;
    }
    return true;
}

string recv_once(int fd) {
    char buf[BUF];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return "";
    buf[n] = '\0';
    return string(buf);
}

bool talk_to_server(const string &msg, string &response) {
    lock_guard<mutex> lk(io_mtx);
    if (!send_all(server_sock, msg)) return false;
    response = recv_once(server_sock);
    return !response.empty();
}

string trim(const string &s) {
    size_t i = s.find_first_not_of(" \t\r\n");
    if (i == string::npos) return "";
    size_t j = s.find_last_not_of(" \t\r\n");
    return s.substr(i, j - i + 1);
}

// 從 List 回覆中找出某 user 的 IP 與 port
bool find_user_in_list(const string &list, const string &user, string &ip, string &port) {
    istringstream iss(list);
    string line;

    // 跳過前三行 (餘額、公鑰、上線人數)
    getline(iss, line);
    getline(iss, line);
    getline(iss, line);

    while (getline(iss, line)) {
        line = trim(line); // 去除尾端 \r 或空白
        if (line.empty()) continue;

        // line 例如：cc#127.0.0.1#4444
        string uname, uip, uport;
        size_t p1 = line.find('#');
        size_t p2 = line.find('#', p1 + 1);
        if (p1 == string::npos || p2 == string::npos) continue;

        uname = line.substr(0, p1);
        uip   = line.substr(p1 + 1, p2 - p1 - 1);
        uport = line.substr(p2 + 1);

        if (uname == user) {
            ip = uip;
            port = uport;
            return true;
        }
    }
    return false;
}

// ------------------------ P2P 收款監聽 ------------------------
// 注意：不要在這個 thread 裡印「正在監聽…」；由主線在登入成功後印一次即可。
// ------------------------ P2P 收款監聽 ------------------------
void listener_thread_fn(int port) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return;

    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ls, (sockaddr *)&addr, sizeof(addr)) < 0) { close(ls); return; }
    if (listen(ls, 8) < 0) { close(ls); return; }

    while (true) {
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int cs = accept(ls, (sockaddr *)&caddr, &clen);
        if (cs < 0) continue;

        // 每筆轉帳開一個獨立執行緒
        thread([cs]() {
            string msg = recv_once(cs);         // 期待格式：A#amount#B
            if (msg.empty()) { close(cs); return; }

            size_t h1 = msg.find('#');
            size_t h2 = msg.find('#', h1 + 1);
            if (h1 == string::npos || h2 == string::npos) { close(cs); return; }

            string sender = msg.substr(0, h1);
            string amount_str = msg.substr(h1 + 1, h2 - h1 - 1);
            string receiver = msg.substr(h2 + 1);

            // 收款人必須是我自己（避免亂打）
            if (receiver != login_user) {
                send_all(cs, "Transfer Failed!");
                close(cs);
                return;
            }

            // 把訊息送給助教 server，不等待回覆（避免死鎖）
            bool ok = false;
            {
                lock_guard<mutex> lk(io_mtx);
                ok = send_all(server_sock, msg);
            }

            if (ok) send_all(cs, "Transfer OK!");
            else send_all(cs, "Transfer Failed!");

            close(cs);
        }).detach();
    }
}


// ------------------------ 主程式 ------------------------
int main(int argc, char *argv[]) {
    if (argc != 3) {
        cerr << "Usage: ./client <ServerIP> <ServerPort>\n";
        return 1;
    }
    string sip = argv[1];
    int sport = stoi(argv[2]);

    // 連線助教 server
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) { cerr << "socket() failed\n"; return 1; }
    sockaddr_in saddr{};
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(sport);
    inet_pton(AF_INET, sip.c_str(), &saddr.sin_addr);

    if (connect(server_sock, (sockaddr *)&saddr, sizeof(saddr)) < 0) {
        cerr << "connect() failed\n"; return 1;
    }
    cout << "Connected to the server!\n";

    while (true) {
        cout << "\n=== 選單 ===\n"
             << "1. REGISTER\n"
             << "2. LOGIN\n"
             << "3. LIST\n"
             << "4. TRANSFER\n"
             << "5. EXIT\n"
             << "請選擇 [1-5]: ";

        int choice;
        if (!(cin >> choice)) {
            cin.clear(); cin.ignore(1024, '\n');
            cout << "指令錯誤，請輸入 1~5。\n";
            continue;
        }
        cin.ignore();

        if (choice == 1) {
            // REGISTER
            cout << "輸入註冊名稱: ";
            string usr; getline(cin, usr); usr = trim(usr);
            if (usr.empty()) { cout << "名稱不可為空。\n"; continue; }

            string pkt = "REGISTER#" + usr;
            string r;
            if (!talk_to_server(pkt, r)) { cout << "傳送失敗。\n"; break; }
            cout << "\n--- Server Response ---\n" << r << "------------------------\n";

            // 若成功註冊，暫存名稱（用於本機提示用；真正驗證還是看 server）
            if (r.find("100 OK") != string::npos) {
                login_user.clear(); // 只是註冊，不代表已登入
            }
        }
        else if (choice == 2) {
            // LOGIN：先檢查帳號存在，再問 port，再正式登入，再啟動監聽
            cout << "輸入使用者名稱: ";
            string usr; getline(cin, usr); usr = trim(usr);
            if (usr.empty()) { cout << "名稱不可為空。\n"; continue; }

            // 先用「usr#0」探測帳號是否存在（不存在會回 220 AUTH_FAIL）
            {
                string probe = usr + "#0";
                string r;
                if (!talk_to_server(probe, r)) { cout << "傳送失敗。\n"; break; }
                if (r.find("220 AUTH_FAIL") != string::npos) {
                    cout << "登入失敗：該帳號尚未註冊。\n";
                    continue;
                }
            }

            // 帳號存在 → 請使用者輸入 port
            cout << "輸入監聽 port (1024~65535): ";
            int p; if (!(cin >> p)) { cin.clear(); cin.ignore(1024, '\n'); cout << "Port 必須是數字。\n"; continue; }
            cin.ignore();
            if (p < 1024 || p > 65535) { cout << "Port 超出範圍。\n"; continue; }

            // 傳送正式登入
            {
                string pkt = usr + "#" + to_string(p);
                string r;
                if (!talk_to_server(pkt, r)) { cout << "傳送失敗。\n"; break; }
                cout << "\n--- Server Response ---\n" << r << "------------------------\n";
                if (r.find("220 AUTH_FAIL") != string::npos) {
                    cout << "登入失敗：驗證錯誤。\n";
                    continue;
                }
            }

            // 登入成功 → 設定狀態並啟動監聽（只印一次提示，避免你遇到的「尾端才出現」）
            login_user = usr;
            listen_port = p;
            logged_in = true;

            if (!listener_started) {
                listener_started = true;
                thread(listener_thread_fn, listen_port).detach();
            }
            cout << "已登入，監聽埠 " << listen_port << " 已啟動。\n";
        }
        else if (choice == 3) {
            // LIST
            if (!logged_in) { cout << "Please login first\n"; continue; }
            string r;
            if (!talk_to_server("List", r)) { cout << "傳送失敗。\n"; break; }
            cout << "\n--- Server Response ---\n" << r << "------------------------\n";
        }
        else if (choice == 4) {
            if (!logged_in) { cout << "Please login first\n"; continue; }
        
            cout << "輸入轉帳格式 (A#amount#B): ";
            string line; getline(cin, line); line = trim(line);
        
            size_t h1 = line.find('#'), h2 = line.find('#', h1 + 1);
            if (h1 == string::npos || h2 == string::npos) { cout << "格式錯誤，例：AA#1000#BB\n"; continue; }
        
            string sender   = trim(line.substr(0, h1));
            string amount   = trim(line.substr(h1 + 1, h2 - h1 - 1));
            string receiver = trim(line.substr(h2 + 1));
        
            if (sender != login_user) { cout << "Sender 必須是目前登入的使用者。\n"; continue; }
            try { if (stod(amount) <= 0) { cout << "金額必須大於 0。\n"; continue; } } catch (...) { cout << "金額必須是數字。\n"; continue; }
        
            // 先向 server 要名單，找出 B 的 IP/Port
            string list_before;
            {
                lock_guard<mutex> lk(io_mtx);
                if (!send_all(server_sock, "List")) { cout << "傳送失敗。\n"; break; }
                list_before = recv_once(server_sock);
            }
            cout << "Auto renew list from tracker before transfer...\n"
                 << list_before << "---------------------\n";
        
            string ip, port;
            if (!find_user_in_list(list_before, receiver, ip, port)) {
                cout << "找不到收款人或對方不在線上。\n"; continue;
            }
        
            int port_num;
            try { port_num = stoi(trim(port)); } catch (...) { cout << "收款人 port 非法：" << port << "\n"; continue; }
        
            // A 直接連到 B（P2P）
            int ps = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in paddr{}; paddr.sin_family = AF_INET; paddr.sin_port = htons(port_num);
            if (inet_pton(AF_INET, ip.c_str(), &paddr.sin_addr) <= 0 || connect(ps, (sockaddr*)&paddr, sizeof(paddr)) < 0) {
                cout << "無法連接到收款人 " << receiver << " (" << ip << ":" << port_num << ")\n"; close(ps); continue;
            }
        
            if (!send_all(ps, line)) { cout << "傳送失敗。\n"; close(ps); continue; }
            string ack = recv_once(ps);  // 期待 "Transfer OK!" or "Transfer Failed!"
            close(ps);
        
            if (ack.find("Transfer OK") != string::npos) {
                cout << "Transfer OK!\n";
                // 成功後再向 server 拉一次最新清單
                string list_after;
                {
                    lock_guard<mutex> lk(io_mtx);
                    send_all(server_sock, "List");
                    list_after = recv_once(server_sock);
                }
                cout << "auto renew list from tracker after transfer...\n"
                     << list_after << "---------------------\n";
            } else {
                cout << "Transfer Failed!\n";
            }
        
            cout << "(按 Enter 回選單)"; cin.get();
            continue;
        }
        
                       
        else if (choice == 5) {
            // EXIT
            string r;
            if (talk_to_server("Exit", r) && !r.empty()) {
                cout << "\n--- Server Response ---\n" << r << "------------------------\n";
            }
            cout << "Bye\n";
            break;
        }
        else {
            cout << "指令錯誤，請輸入 1~5。\n";
        }
    }

    if (server_sock >= 0) close(server_sock);
    return 0;
}
