#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

// ====================== 协议宏定义（和服务器完全对齐） ======================
#define SERVER_IP     "127.0.0.1"
#define SERVER_PORT   2538
#define BUF_SIZE      1024
#define MSG_LEN       4
#define SEP           "|"

// 消息类型（和服务器枚举完全对应）
#define REGISTER     1
#define LOGIN        2
#define GROUP_CHAT   3
#define PRIVATE_CHAT 4
#define ONLINE_LIST  5
#define LOGOUT       6
#define HISTORY      7
#define RESPONSE     8

// ====================== 全局变量 ======================
int sockfd;
char username[32] = {0};
int is_login = 0;
int in_private = 0;
char chat_with[32] = {0};
pthread_mutex_t print_mutex;

// ====================== 工具函数 ======================
// 获取时间
void get_time(char *t) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    sprintf(t, "%02d:%02d", tm->tm_hour, tm->tm_min);
}

// 协议发送
void send_msg(int type, char **params, int cnt) {
    char body[BUF_SIZE] = {0};
    char buf[BUF_SIZE] = {0};
    sprintf(body, "%d", type);
    for (int i = 0; i < cnt; i++) {
        strcat(body, SEP);
        strcat(body, params[i]);
    }
    sprintf(buf, "%04d", (int)strlen(body));
    strcat(buf, body);
    send(sockfd, buf, strlen(buf), 0);
}

// 清屏打印标题
void show_title() {
    system("clear");
    printf("=====================================================\n");
    printf("               📞 TCP群聊聊天室 📞\n");
    printf("=====================================================\n");
    printf("👤 他人消息(左)    🧑‍💻 我的消息(右)    ⏰ 时间\n");
    printf("-----------------------------------------------------\n");
    printf("【操作指南】\n");
    printf("群聊: 直接发消息    私聊: talk 用户名    退出私聊: exit\n");
    printf("注册: /reg 账号 密码    登录: /login 账号 密码\n");
    printf("在线用户: /list    历史记录: /history    退出登录: /logout    退出: /quit\n");
    printf("=====================================================\n\n");
}

// ====================== 接收消息线程 ======================
void *recv_thread(void *arg) {
    char buf[BUF_SIZE], t[16], sender[32], content[BUF_SIZE];
    while (1) {
        memset(buf, 0, BUF_SIZE);
        int n = recv(sockfd, buf, BUF_SIZE-1, 0);
        if (n <= 0) {
            pthread_mutex_lock(&print_mutex);
            printf("\n🔴 服务器断开连接\n");
            pthread_mutex_unlock(&print_mutex);
            exit(0);
        }

        char *msg = buf + MSG_LEN;

        // 隐藏发送成功提示
        if (strstr(msg, "send success") != NULL) continue;

        // 登录成功
        if (strstr(msg, "login success") != NULL) {
            pthread_mutex_lock(&print_mutex);
            printf("\n✅ 登录成功！\n");
            pthread_mutex_unlock(&print_mutex);
            continue;
        }

        // 注销成功
        if (strstr(msg, "logout success") != NULL) {
            pthread_mutex_lock(&print_mutex);
            printf("\n✅ 注销成功！已退出登录\n");
            is_login = 0;
            memset(username, 0, sizeof(username));
            in_private = 0;
            memset(chat_with, 0, sizeof(chat_with));
            pthread_mutex_unlock(&print_mutex);
            continue;
        }

        // 在线用户列表
        if (strstr(msg, "当前在线用户") != NULL) {
            strtok(msg, "|");
            strtok(NULL, "|");
            char *real_content = strtok(NULL, "|");

            if (real_content) {
                pthread_mutex_lock(&print_mutex);
                get_time(t);
                printf("\n\033[1;36m📋 在线用户列表 %s：%s\033[0m\n", t, real_content);
                pthread_mutex_unlock(&print_mutex);
            }
            continue;
        }

        // 历史记录（已修复括号警告）
        if (strstr(msg, "--- 您的聊天记录 ---") != NULL || 
            (strstr(msg, "[") != NULL && strstr(msg, "]") != NULL && strstr(msg, "：") != NULL)) {
            pthread_mutex_lock(&print_mutex);
            printf("\n\033[1;35m📜 聊天记录：\n%s\033[0m\n", msg);
            pthread_mutex_unlock(&print_mutex);
            continue;
        }

        // 系统上下线提示
        if (strstr(msg, "system:") != NULL) {
            pthread_mutex_lock(&print_mutex);
            get_time(t);
            printf("\n\033[1;37m[系统 %s] %s\033[0m\n", t, msg);
            pthread_mutex_unlock(&print_mutex);
            continue;
        }

        // 解析私聊消息
        // 解析私聊消息（适配服务端格式：[私聊] xxx 对你说：xxx）
        // 解析私聊消息（适配服务端格式：[私聊] xxx 对你说：xxx）
          // 解析私聊消息（适配服务端格式：[私聊] xxx 对你说：xxx）
if (strstr(msg, "[私聊]") != NULL) {
    sscanf(msg, "[私聊] %[^ ] 对你说：%[^\n]", sender, content);

    if (!in_private) {
        in_private = 1;
        strcpy(chat_with, sender);
        pthread_mutex_lock(&print_mutex);
        printf("\n🚨 %s 向你发起私聊，已自动进入私聊模式\n", sender);
        pthread_mutex_unlock(&print_mutex);
    }
    pthread_mutex_lock(&print_mutex);
    get_time(t);
    printf("\n\033[1;34m👤 %s [%s] [私]: %s\033[0m\n", t, sender, content);
    pthread_mutex_unlock(&print_mutex);
    continue;
}

      // 解析群聊消息（服务端格式：[群聊] xxx：xxx）
else if (strstr(msg, "[群聊]") != NULL) {
    sscanf(msg, "[群聊] %[^：]：%[^\n]", sender, content);
    pthread_mutex_lock(&print_mutex);
    get_time(t);
    printf("\n\033[1;34m👤 %s [%s]: %s\033[0m\n", t, sender, content);
    pthread_mutex_unlock(&print_mutex);
}
        // 兜底显示
        else {
            pthread_mutex_lock(&print_mutex);
            get_time(t);
            printf("\n\033[1;34m👤 %s %s\033[0m\n", t, msg);
            pthread_mutex_unlock(&print_mutex);
        }

        // 刷新输入框
        pthread_mutex_lock(&print_mutex);
        if (in_private) {
            printf("\n[私聊 → %s] >> ", chat_with);
        } else {
            printf("\n[群聊] >> ");
        }
        fflush(stdout);
        pthread_mutex_unlock(&print_mutex);
    }
}

// ====================== 主函数 ======================
int main() {
    pthread_mutex_init(&print_mutex, NULL);

    // 连接服务器
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("连接失败");
        return 0;
    }

    show_title();
    printf("✅ 连接服务器成功！\n");

    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread, NULL);

    char line[BUF_SIZE], t[16];
    while (1) {
        // 打印输入框
        pthread_mutex_lock(&print_mutex);
        if (in_private) {
            printf("\n[私聊 → %s] >> ", chat_with);
        } else {
            printf("\n[群聊] >> ");
        }
        fflush(stdout);
        pthread_mutex_unlock(&print_mutex);

        fgets(line, sizeof(line), stdin);
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) == 0) continue;

        // 私聊中逻辑
        if (in_private) {
            if (strcmp(line, "exit") == 0) {
                in_private = 0;
                memset(chat_with, 0, sizeof(chat_with));
                pthread_mutex_lock(&print_mutex);
                printf("✅ 已退出私聊，回到群聊\n");
                pthread_mutex_unlock(&print_mutex);
                continue;
            }

            // 自己的私聊消息：右对齐，绿色
            get_time(t);
            pthread_mutex_lock(&print_mutex);
            printf("                                 \033[1;32m🧑‍💻 %s [私] %s\033[0m\n", t, line);
            pthread_mutex_unlock(&print_mutex);

            char *params[3] = {username, chat_with, line};
            send_msg(PRIVATE_CHAT, params, 3);
            continue;
        }

        // 命令处理
        if (line[0] == '/') {
            char cmd[16], user[32], pwd[32];
            if (sscanf(line, "/%s %s %s", cmd, user, pwd) == 3) {
                if (strcmp(cmd, "reg") == 0) {
                    char *params[2] = {user, pwd};
                    send_msg(REGISTER, params, 2);
                } else if (strcmp(cmd, "login") == 0) {
                    strcpy(username, user);
                    is_login = 1;
                    char *params[2] = {user, pwd};
                    send_msg(LOGIN, params, 2);
                }
            }
            else if (strcmp(line, "/list") == 0) {
                if (!is_login) {
                    printf("⚠️  请先登录！\n");
                    continue;
                }
                send_msg(ONLINE_LIST, NULL, 0);
            }
            else if (strcmp(line, "/history") == 0) {
                if (!is_login) {
                    printf("⚠️  请先登录！\n");
                    continue;
                }
                char *params[1] = {username};
                send_msg(HISTORY, params, 1);
            }
            else if (strcmp(line, "/logout") == 0) {
                if (!is_login) {
                    printf("⚠️  请先登录！\n");
                    continue;
                }
                send_msg(LOGOUT, NULL, 0);
            }
            else if (strcmp(line, "/quit") == 0) {
                send_msg(LOGOUT, NULL, 0);
                close(sockfd);
                break;
            }
            continue;
        }

        // 发起私聊
        if (strncmp(line, "talk ", 5) == 0) {
            if (!is_login) {
                printf("⚠️  请先登录！\n");
                continue;
            }
            char target[32];
            strcpy(target, line + 5);
            in_private = 1;
            strcpy(chat_with, target);
            pthread_mutex_lock(&print_mutex);
            printf("✅ 已和 %s 建立私聊\n", target);
            pthread_mutex_unlock(&print_mutex);
            continue;
        }

        // 群聊消息
        if (!in_private && is_login) {
            get_time(t);
            pthread_mutex_lock(&print_mutex);
            printf("                                 \033[1;32m🧑‍💻 %s [%s]: %s\033[0m\n", t, username, line);
            pthread_mutex_unlock(&print_mutex);

            char *params[2] = {username, line};
            send_msg(GROUP_CHAT, params, 2);
        }
    }

    close(sockfd);
    pthread_mutex_destroy(&print_mutex);
    return 0;
}
