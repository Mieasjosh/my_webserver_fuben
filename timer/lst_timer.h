#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

//连接资源结构体成员需要用到定时器类
 //需要前向声明
 class util_timer;

 struct client_data
 {
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
 };

 class util_timer//定时器节点
 {
public:
    util_timer():prev(NULL),next(NULL){}

public:
    time_t expire;//超时时间
    void (* cb_func)(client_data *);//回调函数
    client_data *user_data;//连接资源
    util_timer *prev;//前向定时器
    util_timer *next;//后继定时器
 };

class sort_timer_lst// 定时器链表
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    // 添加定时器节点（插入到合适位置，保证升序）
    void add_timer(util_timer *timer);
    // 调整定时器节点（连接有活动时，重置超时时间，重新排序）
    void adjust_timer(util_timer *timer);
    // 删除定时器节点（连接正常关闭时调用）
    void del_timer(util_timer *timer);
    // 定时检查：处理超时节点（核心方法）
    //主线程处理管道读事件时，调用 timer_handler ()，timer_handler ()再调用 tick ()
    void tick();

private:
    // 私有方法：添加节点（内部调用，处理插入逻辑）
    void add_timer(util_timer *timer,util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}
    void init(int timeslot);//初始化：设置 m_TIMESLOT，同时初始化管道 u_pipefd、注册信号（SIGALRM/SIGTERM）

    int setnoblocking(int fd);//对文件描述符设置非阻塞

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd,int fd,bool one_shot,int TRIGMode);

    //信号处理函数（处理 SIGALRM/SIGTERM），核心是向管道写端写数据，通知主线程
    static void sig_handle(int sig);

    //工具函数：封装 sigaction，注册信号 + 绑定信号处理函数（比如注册 SIGALRM 到 sig_handler）
    void addsig(int sig,void(handler)(int),bool restart=true);

    //定时器核心调度函数：调用 tick () 处理超时连接，并重设 alarm 触发下一次信号
    //epoll 检测到管道读事件（信号处理函数向管道写了数据）时，主线程调用
    void timer_handler();

    //工具函数：向客户端 fd 写入错误信息（比如 404/500）
    void show_error(int connfd,const char *info);
public:
    //全局管道 fd（一对），用于信号处理函数和主线程通信（解决信号处理函数中不能调用 epoll / 复杂逻辑的问题）
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    //全局的 epoll 内核事件表 fd，所有 fd 的 epoll 操作都复用这个值，避免重复创建
    static int u_epollfd;
    //定时周期（比如 5 秒），控制 SIGALRM 信号的触发间隔
    int m_TIMESLOT;

};

//定时器节点超时（expire <当前时间）时，由 tick () 调用
void cb_func(client_data *user_data);
 




#endif