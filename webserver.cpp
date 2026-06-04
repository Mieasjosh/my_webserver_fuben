#include "webserver.h"
#include <sys/stat.h>   // mkdir（创建上传目录）

WebServer::WebServer()
{
    //http_conn类对象
    users=new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path,200);//获取当前工作目录的绝对路径
    char root[6]="/root";
    m_root=(char *)malloc(strlen(server_path)+strlen(root)+1);
    strcpy(m_root,server_path);
    strcat(m_root,root);

    //定时器
    users_timer=new client_data[MAX_FD];

    //上传目录（默认置空，init() 中根据配置赋值）
    m_upload_dir=NULL;
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[0]);
    close(m_pipefd[1]);
    delete [] users;
    delete [] users_timer;
    delete m_pool;
    if (m_upload_dir)
        free(m_upload_dir);
}

void WebServer::init(int port, string user, string password, string databasename, int log_write, int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    /*
    把外部传入的配置（端口、数据库信息、日志模式、触发模式等）赋值给成员变量（如 m_port、m_user、m_TRIGMode）。
    */
    m_port=port;
    m_user=user;
    m_password=password;
    m_databasename=databasename;
    m_sql_num=sql_num;
    m_thread_num=thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
    /*
    根据 m_TRIGMode（0-3），给 m_LISTENTrigmode/m_CONNTrigmode 赋值（0=LT，1=ET），确定 epoll 触发模式。
    */
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    /*
    初始化日志模块：
- 关闭日志（m_close_log=1）则不初始化；
- 开启则根据 m_log_write（0/1）选择同步 / 异步写日志。
    */
    if(0==m_close_log)
    {
        //初始化日志
        if(1==m_log_write)
            Log::get_instance()->init("./Serverlog",m_close_log,2000,800000,800);
        else 
        Log::get_instance()->init("./Serverlog",m_close_log,2000,800000,0);
    }
}

void WebServer::sql_pool()
{
    /*
    1. 初始化数据库连接池（m_connPool->init）；
    2. 调用 http_conn 的 initmysql_result，加载 user 表到内存 map。
    */
    //初始化数据库连接池
    m_connPool=connection_pool::GetInstance();
    m_connPool->init("localhost",m_user,m_password,m_databasename,3306,m_sql_num,m_close_log);

    //初始化数据库读取表
    users->initmysql_result(m_connPool);

    // ========== 上传功能：初始化上传目录 ==========
    // 上传目录位于当前工作目录下的 ./uploads，与 ./root 平级。
    // 不在 root 下是为了隔离开：静态文件是手动部署的，上传文件是运行时动态生成的。
    char server_path[200];
    getcwd(server_path, 200);
    const char *upload_subdir = "/uploads";
    m_upload_dir = (char *)malloc(strlen(server_path) + strlen(upload_subdir) + 1);
    strcpy(m_upload_dir, server_path);
    strcat(m_upload_dir, upload_subdir);

    // 告知 UploadManager 上传目录路径（后续 init_upload 会往里写 .tmp 文件）
    UploadManager::get_instance()->set_upload_dir(m_upload_dir);

    // 如果目录不存在则创建（0755 = owner 可读写执行，group/other 可读执行）
    mkdir(m_upload_dir, 0755);
}

void WebServer::thread_pool()
{
    //创建线程池对象
    m_pool = new threadpool<http_conn>(m_actormodel,m_connPool,m_thread_num);
}

void WebServer::eventListen()
{
    /*
核心是完成 “网络编程基础流程 + epoll 初始化 + 信号处理”：
创建监听套接字（socket），设置 SO_REUSEADDR（端口复用）、SO_LINGER（优雅关闭）；
绑定（bind）、监听（listen）端口；
创建 epoll 内核事件表（epoll_create），将 listenfd 加入 epoll 监听；
创建信号管道（socketpair），设置非阻塞，将读端加入 epoll；
注册信号处理函数（SIGPIPE 忽略、SIGALRM/SIGTERM 自定义处理）；
设置定时器时钟（alarm (TIMESLOT)），初始化工具类全局 fd（管道 /epoll）。
    */

    m_listenfd=socket(PF_INET,SOCK_STREAM,0);
    assert(m_listenfd >=0 );//在调试阶段检查一个条件是否为真。如果条件为假（0），程序会立即终止并输出错误信息

    //优雅关闭连接
    if(0==m_OPT_LINGER)
    {
        struct linger tmp={0,1};
        setsockopt(m_listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));//????????????
    }
    else
    {
        struct linger tmp={1,1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret=0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));//将一段内存区域的所有字节设置为 0。它是 BSD 系统的遗留函数，POSIX 标准推荐使用 memset
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_ANY);
    address.sin_port=htons(m_port);

    int flag=1;
    setsockopt(m_listenfd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag));//允许将当前 socket 绑定到一个已经处于 TIME_WAIT 状态的端口。

    ret=bind(m_listenfd,(struct sockaddr *)&address,sizeof(address));
    assert(ret>=0);
    ret=listen(m_listenfd,5);
    assert(ret>=0);

    utils.init(TIMESLOT);

    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd=epoll_create1(0);
    assert(m_epollfd!=-1);

    utils.addfd(m_epollfd,m_listenfd,false,m_LISTENTrigmode);
    http_conn::m_epollfd=m_epollfd;

    ret=socketpair(AF_UNIX,SOCK_STREAM,0,m_pipefd);
    assert(ret!=-1);
    utils.setnoblocking(m_pipefd[1]);
    utils.addfd(m_epollfd,m_pipefd[0],false,0);

    utils.addsig(SIGPIPE,SIG_IGN);
    utils.addsig(SIGALRM,utils.sig_handle,false);
    utils.addsig(SIGTERM,utils.sig_handle,false);

    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd=m_pipefd;
    Utils::u_epollfd=m_epollfd;
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
/*
1. 初始化新连接的 http_conn 对象（设置 fd、根目录、触发模式等）；
2. 初始化 client_data（绑定 connfd / 客户端地址）；
3. 创建定时器，设置回调函数（关闭连接）、超时时间（当前时间 + 3*TIMESLOT），加入定时器链表。
*/

    users[connfd].init(connfd,client_address,m_root,m_CONNTrigmode,m_close_log,m_user,m_password,m_databasename);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address=client_address;
    users_timer[connfd].sockfd=connfd;
    util_timer *timer=new util_timer;
    timer->user_data=&users_timer[connfd];
    timer->cb_func=cb_func;
    time_t cur=time(NULL);
    timer->expire=cur+3*TIMESLOT;
    users_timer[connfd].timer=timer;
    utils.m_timer_lst.add_timer(timer);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
/*
1. 刷新定时器超时时间（当前时间 + 3*TIMESLOT）；
2. 调整定时器在链表中的位置（保证链表按超时时间有序）；
3. 打印日志（调整定时器）。
*/
    time_t cur=time(NULL);
    timer->expire=cur+3*TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer,int sockfd)
{
/*
1. 执行定时器回调函数（关闭连接、释放资源）；
2. 从定时器链表中删除该定时器；
3. 打印日志（关闭 fd）。
*/
    timer->cb_func(&users_timer[sockfd]);
    if(timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }
    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclientdata()
{
/*
处理新连接（accept）：
- LT 模式（m_LISTENTrigmode=0）：单次 accept；
- ET 模式（m_LISTENTrigmode=1）：循环 accept（ET 需读尽数据）；
- 超过最大连接数则返回错误；
- 调用 timer () 初始化连接和定时器。
*/
    struct sockaddr_in client_address;
    socklen_t client_addrlength=sizeof(client_address);
    if(0==m_LISTENTrigmode)//LT 模式
    {
        int connfd=accept(m_listenfd,(struct sockaddr *)&client_address,&client_addrlength);
        if(connfd<0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if(http_conn::m_user_count>=MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd,client_address);
    }
    else//ET 模式
    {
        while(1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsingal(bool &timeout,bool &stop_server)
{
/*
处理信号事件（从管道读信号）：
1. 读取管道中的信号字节；
2. 解析信号：SIGALRM→设置 timeout=true（定时器超时），SIGTERM→设置 stop_server=true（停止服务器）；
3. 返回是否处理成功。
*/

    int ret=0;
    int sig;
    char singals[1024];
    ret=recv(m_pipefd[0],singals,sizeof(singals),0);
    if(ret==-1)
    {
        return false;
    }
    else if(ret==0)
    {
        return false;
    }
    else
    {
        for(int i=0;i<ret;i++)
        {
            switch(singals[i])
            {
                case SIGALRM:
                {
                    timeout=true;
                    break;
                }
                case SIGTERM:
                {
                    stop_server=true;
                    break;
                }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
/*
处理读事件（客户端发数据）：
- Reactor 模型（m_actormodel=1）：将读任务加入线程池，等待任务完成后调整定时器 / 处理超时；
- Proactor 模型（m_actormodel=0）：先读取数据，再将任务加入线程池，失败则处理超时。
*/
    util_timer *timer=users_timer[sockfd].timer;

    //reactor
    if(1==m_actormodel)
    {
        if(timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users+sockfd,0);

        while(true)
        {
            if(1==users[sockfd].improv)
            {
                if(1==users[sockfd].timer_flag)
                {
                    deal_timer(timer,sockfd);
                    users[sockfd].timer_flag=0;
                }
                users[sockfd].improv=0;
                break;
            }
        }
    }
    else{
        //proactor
        if(users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users+sockfd);

            if(timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer,sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
/*
处理写事件（给客户端发数据）：
- Reactor 模型：将写任务加入线程池，等待完成后调整定时器 / 处理超时；
- Proactor 模型：直接写数据，成功则调整定时器，失败则处理超时。
*/
    util_timer *timer=users_timer[sockfd].timer;
    //reactor
    if(1==m_actormodel)
    {
        if(timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users+sockfd,1);

        while(true)
        {
            if(1==users[sockfd].improv)
            {
                if(1==users[sockfd].timer_flag)
                {
                    deal_timer(timer,sockfd);
                    users[sockfd].timer_flag=0;
                }
                users[sockfd].improv=0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if(users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if(timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer,sockfd);
        }
    }
}

void WebServer::eventLoop()
{
/*
核心是 “epoll_wait 监听事件 → 遍历就绪事件 → 分发给对应处理函数”：
循环调用 epoll_wait，阻塞等待就绪事件；
遍历每个就绪事件：
事件 fd=listenfd：调用 dealclientdata () 处理新连接；
事件是异常（EPOLLRDHUP/EPOLLHUP/EPOLLERR）：处理超时连接（关闭 fd、删除定时器）；
事件 fd = 管道读端：调用 dealwithsignal () 处理信号；
事件是读事件：调用 dealwithread () 处理；
事件是写事件：调用 dealwithwrite () 处理；
若定时器超时（timeout=true）：调用工具类的 timer_handler () 处理所有超时定时器，重置 alarm；
直到 stop_server=true 时退出循环（服务器停止）。
*/

/*
EPOLLIN：表示对应的文件描述符可以读（包括对端SOCKET正常关闭）

EPOLLOUT：表示对应的文件描述符可以写

EPOLLPRI：表示对应的文件描述符有紧急的数据可读（这里应该表示有带外数据到来）

EPOLLERR：表示对应的文件描述符发生错误

EPOLLHUP：表示对应的文件描述符被挂断；

EPOLLET：将EPOLL设为边缘触发(Edge Triggered)模式，这是相对于水平触发(Level Triggered)而言的

EPOLLONESHOT：只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个socket的话，需要再次把这个socket加入到EPOLL队列里
*/
    bool timeout=false;
    bool stop_server=false;

    while(!stop_server)
    {
        int number=epoll_wait(m_epollfd,events,MAX_EVENT_NUMBER,-1);
        if(number<0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for(int i=0;i<number;i++)
        {
            int sockfd=events[i].data.fd;

            //处理新到的客户连接
            if(sockfd==m_listenfd)
            {
                bool flag=dealclientdata();
                if(false==flag)
                    continue;
            }
            //处理异常事件
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer=users_timer[sockfd].timer;
                deal_timer(timer,sockfd);
            }
            //处理信号
            else if((sockfd == m_pipefd[0]) &&  (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsingal(timeout,stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if(events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if(events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        if(timeout)
        {
            utils.timer_handler();

            // ---------- 上传功能：清理超过 1 小时未完成的上传 ----------
            // 场景：客户端传了一半断网了，临时文件和 map 记录需要定期回收。
            // 每个 timer tick（默认 5 秒）触发一次，max_age=3600 秒（1 小时）。
            UploadManager::get_instance()->cleanup_stale(3600);

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}