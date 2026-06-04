#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD=65536;//最大文件描述符
const int MAX_EVENT_NUMBER=10000;//epoll 内核事件表能监听的最大事件数
const int TIMESLOT=5;//最小超时单位,是心跳检测 / 连接超时的基础时间粒度

class WebServer
{
public:
    WebServer();//初始化 http_conn 数组、静态资源根目录、定时器数组。
    ~WebServer();//关闭文件描述符、释放动态分配的内存

    void init(int port,string user,string password,string databasename,int log_write,int opt_linger,int trigmode,int sql_num,int thread_num,int close_log,int actor_model);

    void thread_pool();//创建线程池（初始化 m_pool）
    void sql_pool();//初始化数据库连接池 + 加载数据库用户表到内存
    void log_write();//初始化日志模块（同步 / 异步、开关）
    void trig_mode();//根据 m_TRIGMode 设置监听 / 连接套接字的 epoll 触发模式（LT/ET）
    void eventListen();//网络初始化：创建监听套接字、设置套接字选项、初始化 epoll、注册信号处理。
    void eventLoop();//服务器主循环：epoll_wait 监听事件，分发处理新连接 / 读 / 写 / 信号 / 超时事件。
    void timer(int connfd,struct sockaddr_in client_address);//为新连接创建定时器，绑定连接数据，加入定时器链表
    void adjust_timer(util_timer *timer);//刷新定时器超时时间
    void deal_timer(util_timer *timer,int sockfd);//处理超时连接：关闭 fd、删除定时器、释放资源
    bool dealclientdata();//处理新客户端连接（accept），初始化连接数据和定时器
    bool dealwithsingal(bool &timeout,bool &stop_server);//处理信号事件（从管道读取信号）：SIGALRM（超时）、SIGTERM（停止服务器）
    void dealwithread(int sockfd);//处理读事件：将读任务加入线程池（区分 Reactor/Proactor 模型）
    void dealwithwrite(int sockfd);//处理写事件：将写任务加入线程池（区分 Reactor/Proactor 模型）


public:
    //基础
    int m_port;//服务器监听的端口号
    char *m_root;//服务器静态资源根目录路径,存放 html/css/js 等静态文件。
    char *m_upload_dir;//上���文件存放目录（例如 "./uploads"），启动时初始化
    int m_log_write;//日志写入模式：0 = 同步写日志，1 = 异步写日志
    int m_close_log;//日志开关：0 = 开启日志，1 = 关闭日志。
    int m_actormodel;//事件处理模型：0=Proactor 模型，1=Reactor 模型

    //用于信号处理的管道（socketpair 创建）：将信号（如 SIGALRM）转为 IO 事件，避免 epoll_wait 被信号中断。
    int m_pipefd[2];
    int m_epollfd;
    http_conn *users;//指向 http_conn 对象数组的指针，每个 fd 对应一个 http_conn（处理该连接的 HTTP 请求）

    //数据库相关
    connection_pool *m_connPool;//数据库连接池指针
    string m_user;//登陆数据库用户名
    string m_password;//登陆数据库密码
    string m_databasename;//使用数据库名
    int m_sql_num;//数据库连接池的最大连接数

    //线程池相关
    threadpool<http_conn> *m_pool;//业务处理线程池指针（多线程处理 HTTP 请求，避免单线程阻塞）
    int m_thread_num;//线程池的线程数量

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];//存储 epoll_wait 返回的就绪事件数组

    int m_listenfd;//服务器监听套接字的 fd
    //优雅关闭连接开关：0 = 关闭 SO_LINGER（默认，连接关闭时立即返回，内核处理剩余数据）；1 = 开启 SO_LINGER（等待数据发送完成后再关闭）
    int m_OPT_LINGER;
    //epoll 触发模式组合：0=LT+LT，1=LT+ET，2=ET+LT，3=ET+ET
    int m_TRIGMode;
    int m_LISTENTrigmode;//监听套接字（m_listenfd）的 epoll 触发模式：0=LT，1=ET
    int m_CONNTrigmode;//连接套接字（客户端 connfd）的 epoll 触发模式：0=LT，1=ET

    //定时器相关
    client_data *users_timer;//指向 client_data 数组的指针，每个 fd 对应一个 client_data
    Utils utils;//工具类对象：封装 epoll 操作、信号处理、定时器链表管理等通用功能
};


#endif