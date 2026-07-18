#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <sqlite3.h>
#include <errno.h>
#include <time.h>

// ====================== 协议定义 ======================
#define MSG_LEN 4
#define SEP "|"
#define BUF_SIZE 1024

typedef enum {
    REGISTER = 1,
    LOGIN,
    GROUP_CHAT,
    PRIVATE_CHAT,
    ONLINE_LIST,
    LOGOUT,
    HISTORY,
    RESPONSE
} MsgType;

#define SUCCESS 0
#define FAIL 1

// ====================== 界面美化 ======================
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_RESET   "\033[0m"
#define SEPARATOR "============================================================"

// ====================== 公共配置 ======================
#define SERVER_PORT 2538
#define EPOLL_MAX_EVENTS 1024
#define DB_FILE "chat.db"

// 在线用户链表
typedef struct ClientNode {
    int sockfd;
    char username[32];
    char ip[16];
    int port;
    struct ClientNode *next;
} ClientNode, *ClientList;

ClientList g_client_list = NULL;
pthread_mutex_t g_mutex;
sqlite3 *g_db = NULL;
int g_epollfd = -1;

// ====================== 网络模块 ======================
int socket_init() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        printf(COLOR_RED "[错误] 创建Socket失败：%s\n" COLOR_RESET, strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf(COLOR_RED "[错误] 绑定端口%d失败：%s\n" COLOR_RESET, SERVER_PORT, strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 128) < 0) {
        printf(COLOR_RED "[错误] 监听端口失败：%s\n" COLOR_RESET, strerror(errno));
        close(listen_fd);
        return -1;
    }

    printf(COLOR_GREEN "[成功] 服务已启动，监听端口：%d\n" COLOR_RESET, SERVER_PORT);
    return listen_fd;
}

int epoll_init() {
    int epollfd = epoll_create(1);
    if (epollfd < 0) {
        printf(COLOR_RED "[错误] 创建Epoll实例失败：%s\n" COLOR_RESET, strerror(errno));
        return -1;
    }
    return epollfd;
}

// ====================== 数据库模块 ======================
int db_init() {
    int ret = sqlite3_open(DB_FILE, &g_db);
    if (ret != SQLITE_OK) {
        printf(COLOR_RED "[错误] 打开数据库失败：%s\n" COLOR_RESET, sqlite3_errmsg(g_db));
        return FAIL;
    }

    // 用户表
    const char *create_user = "CREATE TABLE IF NOT EXISTS user ("
                             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                             "username TEXT UNIQUE NOT NULL,"
                             "password TEXT NOT NULL,"
                             "status INT DEFAULT 0);";
    char *err_msg;
    sqlite3_exec(g_db, create_user, NULL, NULL, &err_msg);

    // 聊天记录表
    const char *create_chat = "CREATE TABLE IF NOT EXISTS chat_record ("
                              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                              "type INT NOT NULL,"
                              "sender TEXT NOT NULL,"
                              "receiver TEXT NOT NULL,"
                              "content TEXT NOT NULL,"
                              "create_time TIMESTAMP DEFAULT (datetime('now','localtime')));";
    ret = sqlite3_exec(g_db, create_chat, NULL, NULL, &err_msg);
    if (ret != SQLITE_OK) printf(COLOR_RED "[错误] 创建聊天记录表失败：%s\n" COLOR_RESET, err_msg);

    printf(COLOR_GREEN "[成功] 数据库初始化完成（文件：%s）\n" COLOR_RESET, DB_FILE);
    return SUCCESS;
}

int db_register(const char *user, const char *pwd) {
    const char *sql = "INSERT INTO user (username,password) VALUES (?,?);";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt,1,user,-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,pwd,-1,SQLITE_STATIC);
    int ret = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return ret == SQLITE_DONE ? SUCCESS : FAIL;
}

int db_login(const char *user, const char *pwd) {
    const char *sql = "SELECT * FROM user WHERE username=? AND password=?;";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt,1,user,-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,pwd,-1,SQLITE_STATIC);
    int ret = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (ret != SQLITE_ROW) return FAIL;

    char up[256];
    sprintf(up, "UPDATE user SET status=1 WHERE username='%s';", user);
    sqlite3_exec(g_db, up, NULL, NULL, NULL);
    return SUCCESS;
}

int db_save_chat(int type, const char *sender, const char *receiver, const char *content) {
    const char *sql = "INSERT INTO chat_record (type,sender,receiver,content) VALUES (?,?,?,?);";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, type);
    sqlite3_bind_text(stmt, 2, sender, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, receiver, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, content, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return SUCCESS;
}

int db_get_history(const char *username, char *out, int max_len) {
    const char *sql = "SELECT type,sender,receiver,content,create_time FROM chat_record "
                       "WHERE sender=? OR receiver=? ORDER BY id DESC LIMIT 50;";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt,1,username,-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,username,-1,SQLITE_STATIC);

    memset(out, 0, max_len);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int type = sqlite3_column_int(stmt, 0);
        const char *s = (const char*)sqlite3_column_text(stmt,1);
        const char *r = (const char*)sqlite3_column_text(stmt,2);
        const char *c = (const char*)sqlite3_column_text(stmt,3);
        const char *t = (const char*)sqlite3_column_text(stmt,4);

        char line[512];
        if (type == 1) snprintf(line, sizeof(line), "[%s] [群聊] %s：%s\n", t, s, c);
        else snprintf(line, sizeof(line), "[%s] [私聊] %s→%s：%s\n", t, s, r, c);

        if (strlen(out) + strlen(line) < max_len - 10) strcat(out, line);
    }
    sqlite3_finalize(stmt);
    if (strlen(out) == 0) sprintf(out, "暂无聊天记录");
    return SUCCESS;
}

// ====================== 客户端管理 ======================
ClientList client_list_init() {
    ClientList head = malloc(sizeof(ClientNode));
    head->sockfd = -1;
    head->next = NULL;
    memset(head->username, 0, sizeof(head->username));
    printf(COLOR_GREEN "[成功] 在线用户列表初始化完成\n" COLOR_RESET);
    return head;
}

int client_add(int sockfd, const char *ip, int port) {
    ClientList n = malloc(sizeof(ClientNode));
    n->sockfd = sockfd;
    strcpy(n->ip, ip);
    n->port = port;
    memset(n->username, 0, sizeof(n->username));
    n->next = NULL;

    ClientList p = g_client_list;
    while (p->next) p = p->next;
    p->next = n;
    printf(COLOR_BLUE "[连接] 新客户端接入：%s:%d（SocketFD：%d）\n" COLOR_RESET, ip, port, sockfd);
    return SUCCESS;
}

int client_del(int sockfd) {
    ClientList p = g_client_list, pre = p;
    while (p) {
        if (p->sockfd == sockfd) {
            if (strlen(p->username) > 0) {
                char up[256];
                sprintf(up, "UPDATE user SET status=0 WHERE username='%s';", p->username);
                sqlite3_exec(g_db, up, NULL, NULL, NULL);
                printf(COLOR_BLUE "[断开] 用户 %s 已退出\n" COLOR_RESET, p->username);
            } else {
                printf(COLOR_BLUE "[断开] 未登录客户端已断开（SocketFD：%d）\n" COLOR_RESET, sockfd);
            }
            pre->next = p->next;
            free(p);
            close(sockfd);
            return SUCCESS;
        }
        pre = p;
        p = p->next;
    }
    return FAIL;
}

int client_get_fd(const char *username) {
    ClientList p = g_client_list->next;
    while (p) {
        if (strcmp(p->username, username) == 0) return p->sockfd;
        p = p->next;
    }
    return -1;
}

int client_broadcast(int exclude_fd, const char *msg) {
    ClientList p = g_client_list->next;
    int count = 0;
    while (p) {
        if (p->sockfd != exclude_fd && strlen(p->username) > 0) {
            send(p->sockfd, msg, strlen(msg), 0);
            count++;
        }
        p = p->next;
    }
    printf(COLOR_YELLOW "[广播] 消息已推送给 %d 位在线用户\n" COLOR_RESET, count);
    return SUCCESS;
}

// ====================== 消息解析&封装 ======================
int msg_parse(char *buf, int *type, char **params) {
    char *pos = buf + MSG_LEN;
    *type = atoi(strtok(pos, SEP));
    int i = 0;
    while ((params[i] = strtok(NULL, SEP)) != NULL) i++;
    return i;
}

int msg_pack(int type, int status, const char *content, char *send_buf) {
    char body[BUF_SIZE - MSG_LEN];
    sprintf(body, "%d%s%d%s%s", type, SEP, status, SEP, content ? content : "");
    char len[5];
    sprintf(len, "%04d", (int)strlen(body));
    strcpy(send_buf, len);
    strcat(send_buf, body);
    return SUCCESS;
}

// ====================== 客户端处理线程 ======================
void *handle_client(void *arg) {
    int cfd = *(int*)arg;
    char buf[BUF_SIZE], send_buf[BUF_SIZE];

    while (1) {
        memset(buf, 0, sizeof(buf));
        int n = recv(cfd, buf, sizeof(buf)-1, 0);
        if (n <= 0) {
            pthread_mutex_lock(&g_mutex);
            client_del(cfd);
            epoll_ctl(g_epollfd, EPOLL_CTL_DEL, cfd, NULL);
            pthread_mutex_unlock(&g_mutex);
            break;
        }

        int msg_type;
        char *params[16] = {NULL};
        msg_parse(buf, &msg_type, params);

        pthread_mutex_lock(&g_mutex);
        memset(send_buf, 0, sizeof(send_buf));

        switch (msg_type) {
            case REGISTER: {
                int r = db_register(params[0], params[1]);
                msg_pack(REGISTER, r, r ? "注册失败：用户名已存在" : "注册成功，请登录", send_buf);
                printf(COLOR_YELLOW "[注册] 用户 %s 尝试注册，结果：%s\n" COLOR_RESET, params[0], r ? "失败" : "成功");
                break;
            }
            case LOGIN: {
                int r = db_login(params[0], params[1]);
                if (r == SUCCESS) {
                    ClientList p = g_client_list->next;
                    while (p) {
                        if (p->sockfd == cfd) { strcpy(p->username, params[0]); break; }
                        p = p->next;
                    }
                    char tip[BUF_SIZE];
                    sprintf(tip, "【系统】用户 %s 进入聊天室", params[0]);
                    client_broadcast(cfd, tip);
                    printf(COLOR_YELLOW "[登录] 用户 %s 登录成功\n" COLOR_RESET, params[0]);
                } else {
                    printf(COLOR_YELLOW "[登录] 用户 %s 登录失败：密码错误\n" COLOR_RESET, params[0]);
                }
                msg_pack(LOGIN, r, r ? "登录失败：用户名或密码错误" : "登录成功", send_buf);
                break;
            }
            case GROUP_CHAT: {
                char msg[BUF_SIZE];
                sprintf(msg, "[群聊] %s：%s", params[0], params[1]);
                db_save_chat(1, params[0], "all", params[1]);
                client_broadcast(cfd, msg);
                msg_pack(GROUP_CHAT, SUCCESS, "群消息发送成功", send_buf);
                break;
            }
            case PRIVATE_CHAT: {
                int fd = client_get_fd(params[1]);
                if (fd == -1) {
                    msg_pack(PRIVATE_CHAT, FAIL, "私聊失败：对方不在线", send_buf);
                    printf(COLOR_YELLOW "[私聊] %s 给 %s 发消息失败：对方不在线\n" COLOR_RESET, params[0], params[1]);
                } else {
                    char msg[BUF_SIZE];
                    sprintf(msg, "[私聊] %s 对你说：%s", params[0], params[2]);
                    db_save_chat(2, params[0], params[1], params[2]);
                    send(fd, msg, strlen(msg), 0);
                    msg_pack(PRIVATE_CHAT, SUCCESS, "私聊发送成功", send_buf);
                    printf(COLOR_YELLOW "[私聊] %s 给 %s 发消息成功\n" COLOR_RESET, params[0], params[1]);
                }
                break;
            }
            case ONLINE_LIST: {
                char list[BUF_SIZE] = "当前在线用户：";
                ClientList p = g_client_list->next;
                int user_count = 0;
                while (p) {
                    if (strlen(p->username) > 0) {
                        strcat(list, p->username);
                        strcat(list, " ");
                        user_count++;
                    }
                    p = p->next;
                }
                if (user_count == 0) strcat(list, "暂无");
                msg_pack(ONLINE_LIST, SUCCESS, list, send_buf);
                printf(COLOR_YELLOW "[查询] SocketFD：%d 查询在线用户，共 %d 人\n" COLOR_RESET, cfd, user_count);
                break;
            }
            case HISTORY: {
                char history[BUF_SIZE];
                db_get_history(params[0], history, BUF_SIZE);
                msg_pack(HISTORY, SUCCESS, history, send_buf);
                printf(COLOR_YELLOW "[查询] 用户 %s 拉取聊天记录\n" COLOR_RESET, params[0]);
                break;
            }
            case LOGOUT: {
                char name[32] = {0};
                ClientList p = g_client_list->next;
                while (p) { if (p->sockfd == cfd) { strcpy(name, p->username); break; } p = p->next; }
                
                if (strlen(name) > 0) {
                    char tip[BUF_SIZE];
                    sprintf(tip, "【系统】用户 %s 离开聊天室", name);
                    client_broadcast(cfd, tip);
                }
                
                client_del(cfd);
                epoll_ctl(g_epollfd, EPOLL_CTL_DEL, cfd, NULL);
                msg_pack(LOGOUT, SUCCESS, "退出成功", send_buf);
                send(cfd, send_buf, strlen(send_buf), 0);
                pthread_mutex_unlock(&g_mutex);
                break;
            }
            default:
                msg_pack(RESPONSE, FAIL, "未知指令", send_buf);
                printf(COLOR_YELLOW "[警告] SocketFD：%d 收到未知指令\n" COLOR_RESET, cfd);
                break;
        }

        if (msg_type != LOGOUT) send(cfd, send_buf, strlen(send_buf), 0);
        pthread_mutex_unlock(&g_mutex);
    }
    return NULL;
}

// ====================== 主函数 ======================
int main() {
    printf(COLOR_BLUE "%s\n" COLOR_RESET, SEPARATOR);
    printf(COLOR_GREEN "\t\t Linux 聊天室服务端\n" COLOR_RESET);
    printf(COLOR_BLUE "%s\n" COLOR_RESET, SEPARATOR);

    pthread_mutex_init(&g_mutex, NULL);
    if (db_init() != SUCCESS) {
        printf(COLOR_RED "[错误] 数据库初始化失败，程序退出\n" COLOR_RESET);
        exit(EXIT_FAILURE);
    }
    g_client_list = client_list_init();
    int listen_fd = socket_init();
    if (listen_fd < 0) {
        printf(COLOR_RED "[错误] Socket初始化失败，程序退出\n" COLOR_RESET);
        exit(EXIT_FAILURE);
    }
    g_epollfd = epoll_init();
    if (g_epollfd < 0) {
        close(listen_fd);
        printf(COLOR_RED "[错误] Epoll初始化失败，程序退出\n" COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    struct epoll_event ev, events[EPOLL_MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(g_epollfd, EPOLL_CTL_ADD, listen_fd, &ev);

    printf(COLOR_GREEN "%s\n" COLOR_RESET, SEPARATOR);
    printf(COLOR_GREEN "[运行] 聊天室服务端已启动，等待客户端连接...\n" COLOR_RESET);
    printf(COLOR_GREEN "[提示] 按 Ctrl+C 停止服务\n" COLOR_RESET);
    printf(COLOR_GREEN "%s\n" COLOR_RESET, SEPARATOR);

    while (1) {
        int n = epoll_wait(g_epollfd, events, EPOLL_MAX_EVENTS, -1);
        if (n < 0 && errno != EINTR) {
            printf(COLOR_RED "[错误] Epoll等待事件失败：%s\n" COLOR_RESET, strerror(errno));
            continue;
        }
        
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == listen_fd) {
                struct sockaddr_in addr;
                socklen_t len = sizeof(addr);
                int cfd = accept(listen_fd, (struct sockaddr*)&addr, &len);
                if (cfd < 0) {
                    printf(COLOR_RED "[错误] 接受客户端连接失败：%s\n" COLOR_RESET, strerror(errno));
                    continue;
                }

                char ip[16];
                inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
                int port = ntohs(addr.sin_port);

                ev.events = EPOLLIN;
                ev.data.fd = cfd;
                epoll_ctl(g_epollfd, EPOLL_CTL_ADD, cfd, &ev);

                pthread_mutex_lock(&g_mutex);
                client_add(cfd, ip, port);
                pthread_mutex_unlock(&g_mutex);

                pthread_t tid;
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                pthread_create(&tid, &attr, handle_client, &cfd);
                pthread_attr_destroy(&attr);
            }
        }
    }

    close(listen_fd);
    sqlite3_close(g_db);
    pthread_mutex_destroy(&g_mutex);
    return 0;
}