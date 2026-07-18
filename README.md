# Linux_TCP_ChatRoom
基于Linux C语言实现的TCP网络聊天室，支持Socket、epoll、多线程和SQLite数据库

## 项目功能

- TCP Socket通信
- 多线程pthread
- epoll高并发模型
- SQLite用户数据库
- 用户注册/登录
- 群聊
- 私聊
- 聊天记录保存

## 开发环境

- Ubuntu Linux
- GCC
- SQLite3
- pthread

## 编译运行

### 服务端

进入server目录：

```bash
make
./server