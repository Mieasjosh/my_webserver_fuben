#include "lst_timer.h"
#include "../http/http_conn.h"

//内核定时（alarm）→ 发送SIGALRM → sig_handler写管道 → epoll检测到管道读事件 → 调用timer_handler() → m_timer_lst.tick() → 处理超时连接

sort_timer_lst::sort_timer_lst()
{
    head=NULL;
    tail=NULL;
}

sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp=head;
    while(tmp)
    {
        head=tmp->next;
        delete tmp;
        tmp=head;
    }
}

//添加定时器，内部调用私有成员add_timer
void sort_timer_lst::add_timer(util_timer *timer)
{
    if(!timer)
        return;
    if(!head)
    {
        head=tail=timer;
        return;
    }
    //如果新的定时器超时时间小于当前头部结点
    //直接将当前定时器结点作为头部结点
    if(timer->expire<head->expire)
    {
        timer->next=head;
        head->prev=timer;
        head=timer;
        return;
    }
    add_timer(timer,head);
}

//调整定时器，任务发生变化时，调整定时器在链表中的位置
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if(!timer)
        return;
    util_timer *tmp=timer->next;
    //被调整的定时器在链表尾部
    //定时器超时值仍然小于下一个定时器超时值，不调整
    if(!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    //被调整定时器是链表头结点，将定时器取出，重新插入
    if(timer==head)
    {
        head=head->next;
        head->prev=NULL;
        timer->next=NULL;
        add_timer(timer,head);
    }
    //被调整定时器在内部，将定时器取出，重新插入
    else{
        timer->prev->next=timer->next;
        timer->next->prev=timer->prev;
        add_timer(timer,head);
    }
}

//删除定时器
void sort_timer_lst::del_timer(util_timer *timer)
{
    if(!timer)
        return;
    //链表中只有一个定时器，需要删除该定时器
    if((timer==head)&&(timer==tail))
    {
        head=NULL;
        tail=NULL;
        delete timer;
        return;
    }
    //被删除的定时器为头结点
    if(timer==head)
    {
        head=head->next;
        head->prev=NULL;
        delete timer;
        return;
    }
    //被删除的定时器为尾结点
    if(timer==tail)
    {
        tail=tail->prev;
        tail->next=NULL;
        delete timer;
        return;
    }
    //被删除的定时器在链表内部，常规链表结点删除
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;

    delete timer;
}

void sort_timer_lst::tick()
{
    if(!head)
        return;
    
    //获取当前时间
    time_t cur=time(NULL);
    util_timer *tmp=head;
    
    //遍历定时器链表
    while(tmp)
    {
        //链表容器为升序排列
        //当前时间小于定时器的超时时间，后面的定时器也没有到期
        if(cur<tmp->expire)
            break;
        //当前定时器到期，则调用回调函数，执行定时事件
        tmp->cb_func(tmp->user_data);

        //将处理后的定时器从链表容器中删除，并重置头结点
        head=tmp->next;
        if(head)
        {
            head->prev=NULL;
        }
        delete tmp;
        tmp=head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer,util_timer *lst_head)//就不考虑会不会插入到头节点的位置？？？
{
    util_timer *prev=lst_head;
    util_timer *tmp=prev->next;

    while(tmp)
    {
        if(timer->expire < tmp->expire)
        {
            prev->next=timer;
            tmp->prev=timer;
            timer->next=tmp;
            timer->prev=prev;
            break;
        }
        prev=tmp;
        tmp=tmp->next;
    }
    if(!tmp)
    {
        prev->next=timer;
        timer->next=NULL;
        timer->prev=prev;
        tail=timer;
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT=timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnoblocking(int fd)
{
    int old_option=fcntl(fd,F_GETFL);
    int new_opton=old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_opton);
    return old_option;//为什么要返回旧的？？？？？？？？？？？？
    /*为什么返回旧值？
    这是「预留扩展性」：如果后续需要恢复 fd 的原始状态（比如临时设置非阻塞，处理完 IO 后改回阻塞），
    就可以用返回的 old_option 重新设置*/
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd,int fd,bool one_shot,int TRIGMode)
{
    epoll_event event;
    event.data.fd=fd;

    if(1==TRIGMode)// ET模式（边缘触发）
    {
        event.events=EPOLLIN |EPOLLET |EPOLLRDHUP;
    }
    else// LT模式（水平触发）
        event.events=EPOLLIN | EPOLLRDHUP;

    if(one_shot)// 开启EPOLLONESHOT
        event.events |= EPOLLONESHOT;

    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnoblocking(fd);
    
    /*EPOLLRDHUP：检测对方关闭连接（比如客户端断开），避免无效读写；
    EPOLLONESHOT：确保一个 fd 的事件只能被一个线程处理（避免多线程竞争同一个 fd 的事件）；*/
}

//信号处理函数
void Utils::sig_handle(int sig)
{

    /*为什么保存 / 恢复 errno？
    信号处理函数可能打断主线程的系统调用（比如 read()），系统调用会修改 errno。保存 errno 是为了保证「函数可重入」，避免主线程后续判断错误（比如主线程本来 errno=0，被信号处理函数改了，导致逻辑出错）。*/

    /*为什么只发送 1 个字节？
    这里的核心目的是「通知」而非「传递大量数据」：
    信号处理函数不能做复杂操作（比如直接调用 timer_handler()），因为信号是异步的，可能打断主线程的关键逻辑（比如 epoll_wait、write），导致「不可重入」问题；
    管道的作用是「把信号事件转换成 epoll 能处理的 IO 事件」（epoll 无法直接监听信号），只要往管道写 1 个字节，就能触发管道读端的 EPOLLIN 事件，主线程从 epoll_wait 唤醒后再处理定时器逻辑；
    发送 1 个字节足够：因为我们只需要 “告诉主线程有信号来了”，不需要传递信号的具体值（也可以传，但 1 字节够存 sig 的低 8 位，满足需求）。*/

    //为保证函数的可重入性，保留原来的errno
    int save_errno=errno;
    int msg=sig;
    send(u_pipefd[1],(char *)&msg,1,0);//这一行也没理解，为什么只发送一个字节

    errno=save_errno;
}

//设置信号函数
//封装 sigaction 系统调用，统一配置信号的处理规则（指定处理函数、自动重启被打断的系统调用、屏蔽嵌套信号），避免重复写冗余代码，同时保证信号处理的安全性。
void Utils::addsig(int sig,void (handler)(int),bool restart)
{
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));//初始化结构体

    //信号处理函数中仅仅发送信号值，不做对应逻辑处理
    sa.sa_handler = handler;
    if(restart)
        sa.sa_flags |= SA_RESTART;// 被信号打断的系统调用自动重启
    /*主线程会调用 epoll_wait() 阻塞等待事件，如果信号来了，会打断 epoll_wait()（返回 -1，errno=EINTR）。设置 SA_RESTART 后，被打断的 epoll_wait() 会自动重启，不需要主线程手动处理。*/

    //将所有信号添加到信号集中.信号处理期间，屏蔽所有其他信号,避免信号嵌套
    sigfillset(&sa.sa_mask);

    //执行sigaction函数
    assert(sigaction(sig,&sa,NULL)!=-1);
}

//定时器的「核心处理入口」,调用定时器链表的 tick() 方法，遍历链表中所有定时器，检查是否超时
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);//重新设置内核定时，让系统每隔 m_TIMESLOT 秒再次发送 SIGALRM 信号
}

void Utils::show_error(int connfd,const char *info)
{
    send(connfd,info,strlen(info),0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;

//定时器的「超时回调函数」，是 sort_timer_lst.tick() 检测到超时时执行的逻辑（每个定时器节点会绑定这个回调）。
void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd,EPOLL_CTL_DEL,user_data->sockfd,0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}