#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string,string> users;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql=NULL;
    connectionRAII(&mysql,connPool);

    //mysql_query:向 MySQL 服务器执行传入的 SQL 语句,成功返回0，失败非0
    //在user表中检索username，passwd数据，浏览器端输入
    if(mysql_query(mysql,"SELECT username ,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));  //还没写LOG，以后再加上  --已经在六月一号加上
    }

    //mysql_store_result：MySQL C API 函数，作用是把 MySQL 服务器返回的查询结果集，一次性完整读取到客户端内存中；这里的客户端指的是web服务器
    //对比：还有mysql_use_result（逐行读取结果，适合大结果集），但user表数据量小，mysql_store_result效率更高
    //从表中检索完整的结果集
    MYSQL_RES *result=mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields=mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields=mysql_fetch_field(result);

    //mysql_fetch_row:从结果集中读取下一行数据； 返回值：MYSQL_ROW（本质是char**，字符串数组），每一个元素对应一行中的一列；
    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while(MYSQL_ROW row=mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1]=temp2;
    }
}

//对文件描述符设置非阻塞
int setnoblocking(int fd)
{
    int old_option=fcntl(fd,F_GETFL);
    int new_option=old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT,one_shot=true表示仅仅被单个线程处理
void addfd(int epollfd,int fd,bool one_shot,int TRIGMode)
{
    epoll_event event;
    event.data.fd=fd;

    if(1==TRIGMode)//ET
        event.events=EPOLLIN | EPOLLET | EPOLLRDHUP;
    else//LT
        event.events=EPOLLIN | EPOLLRDHUP;

    if(one_shot)
        event.events |= EPOLLONESHOT;
    /*EPOLLRDHUP：检测对方关闭连接（比如客户端断开），避免无效读写；
    EPOLLONESHOT：确保一个 fd 的事件只能被一个线程处理（避免多线程竞争同一个 fd 的事件）；*/
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnoblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd,int fd,int ev,int TRIGMode)
{
    epoll_event event;
    event.data.fd=fd;

    if(1==TRIGMode)
        event.events =ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events =ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

int http_conn::m_user_count=0;
int http_conn::m_epollfd=-1;//初始化为-1，-1表示未打开

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if(real_close && (m_sockfd !=-1))
    {
        printf("close%d\n",m_sockfd);
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址(对于这个函数的调用者和调用时机还不理解)
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,int close_log, string user, string passwd, string sqlname)
{
    m_sockfd=sockfd;
    m_address=addr;//m_address的定义不是指针，可是这里传入的参数确是地址？？？？？？？？？？？？？？？???????
    
    addfd(m_epollfd,sockfd,true,m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root=root;
    m_TRIGMode=TRIGMode;
    m_close_log=close_log;

    strcpy(sql_user,user.c_str());
    strcpy(sql_passwd,passwd.c_str());
    strcpy(sql_name,sqlname.c_str());

    init();
}

//初始化新接受的连接,check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    // ---------- 上传功能：每次新请求时重置上传相关状态 ----------
    m_upload_streaming = false;
    m_upload_content_len = 0;
    m_upload_chunk_offset = 0;
    memset(m_upload_filename, '\0', sizeof(m_upload_filename));
    memset(m_upload_response_body, '\0', sizeof(m_upload_response_body));

    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容,返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for(;m_checked_idx < m_read_idx;++m_checked_idx)
    {
        //temp为将要分析的字节
        temp=m_read_buf[m_checked_idx];

        //如果当前是\r字符，则有可能会读取到完整行
        if(temp=='\r')
        {
            //下一个字符达到了buffer结尾，则接收不完整，需要继续接收
            if((m_checked_idx+1)==m_read_idx)
                return LINE_OPEN;
            //下一个字符是\n，将\r\n改为\0\0
            else if(m_read_buf[m_checked_idx+1]=='\n')
            {
                m_read_buf[m_checked_idx++]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            //如果都不符合，则返回语法错误
            return LINE_BAD;
        }
        //如果当前字符是\n，也有可能读取到完整行,一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        else if(temp=='\n')
        {
            //前一个字符是\r，则接收完整
            if(m_checked_idx>1 && m_read_buf[m_checked_idx-1]=='\r')
            {
                m_read_buf[m_checked_idx-1]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //并没有找到\r\n，需要继续接收
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接,非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if(m_read_idx>=READ_BUFFER_SIZE)
        return false;
    int bytes_read=0;

    //LT读取数据
    if(0==m_TRIGMode)
    {
        bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        m_read_idx+=bytes_read;
        if(bytes_read<=0)
        {
            return false;
        }
        return true;
    }
    //ET读数据
    else{
        while(true)
        {
            bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
            //ET 模式循环接收的 “终止条件” 是「recv 返回 -1 + errno=EAGAIN/EWOULDBLOCK」（当前数据读完），而不是「bytes_read==0」—— 后者是连接断了，属于失败场景。
            if(bytes_read==-1)
            {
                //非阻塞ET模式下，需要一次性将数据读完
                if(errno==EAGAIN || errno==EWOULDBLOCK)
                    break;
                return false;
            }
            else if(bytes_read==0)
            {
                return false;
            }
            m_read_idx+=bytes_read;
        }
        return true;
    }
}


//解析http请求行，获得请求方法，目标url及http版本号    text:指向当前待解析的 HTTP 报文片段起始位置，是解析函数的统一输入源
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    /*
    strchr:查单个字符第一次出现的位置
    strrchr:找字符最后一次出现的位置
    strpbrk:查多个字符里任意一个
    strstr:查一整段连续字符串
    strcasecmp:忽略大小写比较字符串
    srencasecmp:忽略大小写比较前n个字符
    atol:字符串转long
    atoi:字符串转成int
    strspn(const char *str,const char *accept):计算字符串开头连续、仅由指定字符集里字符组成的长度
    */

   //在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。
    //请求行中最先含有空格和\t任一字符的位置并返回
    m_url = strpbrk(text," \t");

    if(!m_url)//如果没有空格或\t，则报文格式有误
    {
        return BAD_REQUEST;
    }

    //将该位置改为\0，用于将前面数据取出
    *m_url++='\0';

    //取出数据，并通过与GET和POST比较，以确定请求方式
    char *method=text;
    if(strcasecmp(method,"GET")==0)
        m_method=GET;
    else if(strcasecmp(method,"POST")==0)
    {
        m_method=POST;
        cgi=1;
    }
    else
        return BAD_REQUEST;
    
    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    m_url+=strspn(m_url," \t");

    //使用与判断请求方式的相同逻辑，判断HTTP版本号
    m_version=strpbrk(m_url," \t");
    if(!m_version)
        return BAD_REQUEST;
    *m_version++='\0';
    m_version+=strspn(m_version," \t");

    //仅支持HTTP/1.1
    if(strcasecmp(m_version,"HTTP/1.1")!=0)
        return BAD_REQUEST;

    //对请求资源前7个字符进行判断
    //这里主要是有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    if(strncasecmp(m_url,"http://",7)==0)
    {
        m_url+=7;//跳过请求行中请求资源的http://
        m_url=strchr(m_url,'/');
    }

    //同样增加https情况
    if(strncasecmp(m_url,"https://",8)==0)
    {
        m_url+=8;
        m_url=strchr(m_url,'/');
    }

    //一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if(!m_url || m_url[0]!='/')
        return BAD_REQUEST;
    
    //当url为/时，显示欢迎界面
    if(strlen(m_url)==1)
        strcat(m_url,"judge.html");
    
    //请求行处理完毕，将主状态机转移处理请求头
    m_check_state=CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    /*
    strchr:查单个字符第一次出现的位置
    strrchr:找字符最后一次出现的位置
    strpbrk:查多个字符里任意一个
    strstr:查一整段连续字符串
    strcasecmp:忽略大小写比较字符串
    srencasecmp:忽略大小写比较前n个字符
    atol:字符串转long
    atoi:字符串转成int
    strspn(const char *str,const char *accept):计算字符串开头连续、仅由指定字符集里字符组成的长度
    */

    //判断是空行还是请求头
    if(text[0]=='\0')
    {
        //判断是GET还是POST请求
        if(m_content_length!=0)
        {
            //POST需要跳转到消息体处理状态
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    //解析请求头部连接字段
    else if(strncasecmp(text,"Connection:", 11)==0)
    {
        text+=11;
        //跳过空格和\t字符
        text+=strspn(text," \t");
        if(strcasecmp(text,"keep-alive")==0)
        {
            m_linger=true;
        }
    }
    //解析请求头部内容长度字段
    else if(strncasecmp(text, "Content-length:", 15) == 0)
    {
        text+=15;
        text+=strspn(text," \t");
        m_content_length=atol(text);
    }
    //解析请求头部HOST字段
    else if(strncasecmp(text, "Host:", 5) == 0)
    {
        text+=5;
        text+=strspn(text," \t");
        m_host=text;
    }
    // ---------- 上传功能：解析自定义头部 X-Filename（文件名） ----------
    else if (strncasecmp(text, "X-Filename:", 11) == 0)
    {
        text += 11;                                    // 跳过 "X-Filename:"
        text += strspn(text, " \t");                  // 跳过冒号后的空格/tab
        strncpy(m_upload_filename, text,
                sizeof(m_upload_filename) - 1);        // 安全拷贝，保留末尾 \0
    }
    // ---------- 上传功能：解析自定义头部 X-Offset（分块偏移量） ----------
    else if (strncasecmp(text, "X-Offset:", 9) == 0)
    {
        text += 9;                                     // 跳过 "X-Offset:"
        text += strspn(text, " \t");                  // 跳过冒号后的空格/tab
        m_upload_chunk_offset = atol(text);            // 字符串转 long
    }
    else
        LOG_INFO("oop!unknow header: %s", text);
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    //判断buffer中是否读取了消息体
    if(m_read_idx>=(m_content_length+m_checked_idx))
    {
        text[m_content_length]='\0';
        //POST请求中最后为输入的用户名和密码
        m_string=text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}


/*
对于POST请求，只有parse_content(text)返回NO_REQUEST才会设置line_status=LINE_OPEN;同时循环条件也需要&& line_status == LINE_OK，这样子就可以保证，虽然读缓冲区还没有接收到网络传来完整的POST请求消息体，但是不会一直死循环。而是停下来，等待主线程接收到剩下的数据再继续解析POST请求消息体。而http_conn连接对象会对应每一个客户端，下一次客户端有数据来，然后把新收到的数据追加到m_read_buf，epoll又会插入任务队列，再调用process_read。值得注意的是，每次line_status都会重新初始化
*/
http_conn::HTTP_CODE http_conn::process_read()
{
    //初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text=0;

    //parse_line为从状态机的具体实现
    while((m_check_state==CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line())==LINE_OK))
    {
        text=get_line();

        //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line=m_checked_idx;
        //LOG_INFO("%s", text);!!!!!!!!!!!

        //主状态机的三种状态转移逻辑
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
            //解析请求行
            ret=parse_request_line(text);
            if(ret==BAD_REQUEST)
                return BAD_REQUEST;
            break;
        case CHECK_STATE_HEADER:
            //解析请求头
            ret=parse_headers(text);
            if(ret==BAD_REQUEST)
                return BAD_REQUEST;
            //完整解析GET请求后，跳转到报文响应函数
            else if(ret==GET_REQUEST)
            {
                return do_request();
            }
            break;
        case CHECK_STATE_CONTENT:
            //解析消息体
            ret=parse_content(text);
            //完整解析POST请求后，跳转到报文响应函数
            if(ret==GET_REQUEST)
                return do_request();

            //解析完消息体即完成报文解析，避免再次进入循环，更新line_status
            line_status=LINE_OPEN;
            break;
            
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// ===========================================================================
// json_get_string —— 简易 JSON 字符串值提取
// ===========================================================================
// 设计初衷：项目没有引入任何 JSON 库（如 jsoncpp），而上传 API 只需要解析
// 非常简单的 JSON 如 {"filename":"test.mp4","totalSize":1024,"md5":"abc..."}。
// 用一个轻量级的 strstr + 字符查找实现，避免引入第三方依赖。
//
// 原理：
//   构造搜索模式 "\"key\":\"" → strstr 查找 → 跳过前缀 → strchr 找结尾引号
//   在结尾引号处写 '\0' 截断 → 返回指向值的指针
//
// 限制：
//   ① 不支持转义字符（如 \" 或 \n）——但文件名不含这些，够用
//   ② 不支持嵌套的 JSON 对象或数组
//   ③ 会修改源字符串（截断），所以传入的 json 必须是可写缓冲区
char *http_conn::json_get_string(char *json, const char *key)
{
    // 构造搜索模式，例如 key="filename" → 搜索 "\"filename\":\""
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":\"", key);

    char *start = strstr(json, search);
    if (start == NULL)
        return NULL;               // 没找到这个 key

    start += strlen(search);       // start 现在指向 value 的第一个字符

    // 找到 value 结束的引号
    char *end = strchr(start, '"');
    if (end == NULL)
        return NULL;               // JSON 格式错误：没有闭合引号
    *end = '\0';                   // 截断，让 start 成为一个 C 字符串

    return start;
}

// ===========================================================================
// json_get_long —— 简易 JSON 数值提取
// ===========================================================================
// 类似 json_get_string，但 key 后面跟的是数字而非引号字符串。
// 搜索模式 "\"key\":" → atol 转整数
long http_conn::json_get_long(char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);

    char *start = strstr(json, search);
    if (start == NULL)
        return -1;                 // 没找到这个 key

    start += strlen(search);       // start 指向数字的起始位置
    return atol(start);            // atol 在遇到非数字字符时自动停止
}

// ===========================================================================
// do_request —— URL 路由 + 静态文件服务（现有逻辑 + 上传路由）
// ===========================================================================
http_conn::HTTP_CODE http_conn::do_request()//这个函数由于涉及登陆注册，还没看懂，到时候要回头！！！！！！
{
    //将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file,doc_root);
    int len=strlen(doc_root);

    //找到m_url中/的位置
    const char *p=strrchr(m_url,'/');

    // =====================================================================
    // 上传功能路由：拦截所有以 /upload/ 开头的 POST 请求
    // =====================================================================
    // 必须在 登录/注册路由 之前处理，否则 /upload/ 会被误判为静态文件请求。
    // 三个子端点：
    //   /upload/init     → 初始化上传，返回已收字节数（断点续传入口）
    //   /upload/chunk    → 接收一个二进制分块，流式写入磁盘
    //   /upload/complete → 完成上传，MD5 校验并 rename
    if (strncmp(m_url, "/upload/", 8) == 0)
    {
        const char *action = m_url + 8;   // action 指向 "init" / "chunk" / "complete"

        // ---- /upload/chunk —— 二进制分块上传 ----
        if (strcmp(action, "chunk") == 0)
        {
            // 必须提供文件名和长度头部，否则无法处理
            if (m_upload_filename[0] == '\0' || m_content_length <= 0)
                return BAD_REQUEST;

            // 标记为"流式接收"模式：body 是二进制数据，不经过 m_read_buf 解析，
            // 而是由 receive_file_chunk() 直接从 socket recv → pwrite 写磁盘
            m_upload_streaming = true;
            m_upload_content_len = m_content_length;
            return UPLOAD_STREAMING;   // process() 收到后会调用 receive_file_chunk()
        }

        // ---- /upload/init 和 /upload/complete —— JSON 请求 ----
        // 这两个端点都有 JSON body，已经由 parse_content() 存入 m_string
        if (m_string == NULL || m_string[0] == '\0')
            return BAD_REQUEST;    // 缺少 body

        // ---- /upload/init —— 初始化上传 ----
        if (strcmp(action, "init") == 0)
        {
            char *filename   = json_get_string(m_string, "filename");
            long  total_size = json_get_long(m_string, "totalSize");
            char *md5        = json_get_string(m_string, "md5");

            if (filename == NULL || total_size <= 0 || md5 == NULL)
            {
                snprintf(m_upload_response_body, sizeof(m_upload_response_body),
                         "{\"status\":\"error\",\"msg\":\"缺少必填字段: filename/totalSize/md5\"}");
                return UPLOAD_RESPONSE;
            }

            // ---- 安全检查：防止目录穿越攻击 ----
            // 客户端可能传 "../../etc/passwd" 试图写到网站目录之外。
            // strrchr 找到最后一个 '/' 或 '\'，只取文件名部分（basename）
            char safe_name[256];
            {
                const char *base = strrchr(filename, '/');
                base = (base != NULL) ? base + 1 : filename;   // 跳过 '/'
                const char *bs = strrchr(base, '\\');          // Windows 路径也要防
                base = (bs != NULL) ? bs + 1 : base;
                strncpy(safe_name, base, sizeof(safe_name) - 1);
                safe_name[sizeof(safe_name) - 1] = '\0';
            }

            if (safe_name[0] == '\0')
            {
                snprintf(m_upload_response_body, sizeof(m_upload_response_body),
                         "{\"status\":\"error\",\"msg\":\"文件名无效\"}");
                return UPLOAD_RESPONSE;
            }

            std::string error;
            UploadManager *mgr = UploadManager::get_instance();
            if (mgr->init_upload(safe_name, (size_t)total_size, md5, error))
            {
                // 成功：返回已接收字节数（新上传为 0，续传为已有大小）
                const UploadMeta *meta = mgr->get_upload(safe_name);
                size_t received = (meta != NULL) ? meta->received_bytes : 0;
                snprintf(m_upload_response_body, sizeof(m_upload_response_body),
                         "{\"status\":\"ok\",\"received\":%zu}", received);
            }
            else
            {
                snprintf(m_upload_response_body, sizeof(m_upload_response_body),
                         "{\"status\":\"error\",\"msg\":\"%s\"}", error.c_str());
            }
            return UPLOAD_RESPONSE;
        }

        // ---- /upload/complete —— 完成上传 ----
        if (strcmp(action, "complete") == 0)
        {
            char *filename = json_get_string(m_string, "filename");

            if (filename == NULL)
            {
                snprintf(m_upload_response_body, sizeof(m_upload_response_body),
                         "{\"status\":\"error\",\"msg\":\"缺少 filename 字段\"}");
                return UPLOAD_RESPONSE;
            }

            // 同样的安全过滤
            char safe_name[256];
            {
                const char *base = strrchr(filename, '/');
                base = (base != NULL) ? base + 1 : filename;
                const char *bs = strrchr(base, '\\');
                base = (bs != NULL) ? bs + 1 : base;
                strncpy(safe_name, base, sizeof(safe_name) - 1);
                safe_name[sizeof(safe_name) - 1] = '\0';
            }

            std::string error;
            UploadManager *mgr = UploadManager::get_instance();
            if (mgr->complete_upload(safe_name, error))
            {
                // 校验成功：返回最终文件的 MD5
                char final_path[512];
                snprintf(final_path, sizeof(final_path), "%s/%s",
                         mgr->get_upload(safe_name) ? "uploads" : "", safe_name);
                // 注意：complete_upload 成功后记录已从 map 中删除，这里用一个
                // 固定路径重新计算 MD5（或者让 complete_upload 返回 MD5）
                // 简化处理：返回成功的 status
                snprintf(m_upload_response_body, sizeof(m_upload_response_body),
                         "{\"status\":\"ok\",\"msg\":\"上传完成，MD5 校验通过\"}");
            }
            else
            {
                snprintf(m_upload_response_body, sizeof(m_upload_response_body),
                         "{\"status\":\"error\",\"msg\":\"%s\"}", error.c_str());
            }
            return UPLOAD_RESPONSE;
        }

        // 未知的 /upload/ 子路径 → 400
        snprintf(m_upload_response_body, sizeof(m_upload_response_body),
                 "{\"status\":\"error\",\"msg\":\"未知的上传操作: %s\"}", action);
        return UPLOAD_RESPONSE;
    }

    //处理cgi，实现登录和注册校验
    if(cgi==1 && (*(p+1)=='2' || *(p+1)=='3'))
    {
        //根据标志判断是登录检测还是注册检测
        char flag=m_url[1];

        char *m_url_real=(char *)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/");
        strcat(m_url_real,m_url+2);
        strncpy(m_real_file+len,m_url_real,FILENAME_LEN-len-1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100],passwd[100];
        int i;
        for(i=5;m_string[i]!='&';++i)
            name[i-5]=m_string[i];
        name[i-5]='\0';

        int j=0;
        for(i=i+10;m_string[i]!='\0';++i,++j)
            passwd[j]=m_string[i];
        passwd[j]='\0';

        if(*(p+1)=='3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert=(char *)malloc(sizeof(char)*200);
            strcpy(sql_insert,"INSERT INTO user(username,passwd) VALUES(");
            strcat(sql_insert,"'");
            strcat(sql_insert,name);
            strcat(sql_insert,"', '");
            strcat(sql_insert,passwd);
            strcat(sql_insert,"')");

            if(users.find(name) == users.end())
            {
                //向数据库中插入数据时，需要通过锁来同步数据
                m_lock.lock();
                int res=mysql_query(mysql,sql_insert);
                users.insert(pair<string,string>(name,passwd));
                m_lock.unlock();

                if(!res)//校验成功，跳转登录页面
                    strcpy(m_url,"/log.html");
                else //校验失败，跳转注册失败页面
                    strcpy(m_url,"/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
            //如果是登录，直接判断
            //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
            else if(*(p+1)=='2')
            {
                if(users.find(name)!=users.end() && users[name] == passwd)
                {
                    strcpy(m_url,"/welcome.html");
                }
                else
                    strcpy(m_url, "/logError.html");
            }
    }

    //如果请求资源为/0，表示跳转注册界面
    if(*(p+1)=='0')
    {
        char *m_url_real=(char *)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/register.html");

        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));
        free(m_url_real);
    }
    //如果请求资源为/1，表示跳转登录界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')//图片页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')//视频页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')//关注页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if(stat(m_real_file,&m_file_stat)<0)
        return NO_RESOURCE;

    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if(!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if(S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file,O_RDONLY);
    m_file_address=(char *)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd);
    //表示请求文件存在，且可以访问
    return FILE_REQUEST;


    /*
传统read()+write()（4 次拷贝）：
磁盘文件 → 内核页缓存（第 1 次） → 用户态缓冲区（第 2 次） → 内核发送缓冲区（第 3 次） → 网卡（第 4 次）
mmap 方式（3 次拷贝，少 1 次核心拷贝）：
磁盘文件 → 内核页缓存（第 1 次） → 映射到用户态虚拟内存（无拷贝，仅建立地址映射） → 内核发送缓冲区（第 2 次） → 网卡（第 3 次）

如果用read()读取 100MB 的 HTML 文件，read()会试图把整个文件读到用户缓冲区，直接占用 100MB 用户内存；而 mmap 是「按需映射」：
只有当你访问虚拟内存中某一页（比如 4KB）的地址时，内核才会把磁盘上对应的 4KB 数据加载到内存（缺页中断）；
    */
}

// ===========================================================================
// receive_file_chunk —— 流式接收上传分块的二进制 body
// ===========================================================================
// 这是整个上传功能最核心的方法。它不经过 m_read_buf（2048 字节太小），
// 而是直接从 socket recv 读取数据，然后 pwrite 写入磁盘临时文件。
//
// 调用前提：parse_headers() 已解析了 X-Filename、X-Offset、Content-Length。
//           do_request() 确定路由为 /upload/chunk，返回 UPLOAD_STREAMING。
//           process() 检测到 UPLOAD_STREAMING 后调用本方法。
//
// 流程：
//   ① 处理 m_read_buf 中 header 后面已经读进来的 body 数据（如果有的话）
//   ② 循环 recv 直到收够 Content-Length 字节，每次 8KB 栈缓冲区
//   ③ 设置 30 秒 SO_RCVTIMEO，防止卡死的客户端永久占用工作线程
//   ④ 全部收完后构造 JSON 响应到 m_upload_response_body
//
// 返回值：true=成功（JSON 响应已就绪），false=失败（连接中断、磁盘错误等）
bool http_conn::receive_file_chunk()
{
    UploadManager *mgr = UploadManager::get_instance();

    // 剩余需要接收的字节数（不断减少直到 0）
    long remaining = m_upload_content_len;

    // 当前分块在文件中的写入偏移量
    long offset = m_upload_chunk_offset;

    // ============================================================
    // 步骤 1：处理 m_read_buf 中已经"偷跑"进来的 body 字节
    // ============================================================
    // HTTP 数据是流式的——socket recv 可能把 headers 后面的部分 body 也一起
    // 读进来了。m_checked_idx 指向 headers 结束后的位置（\r\n\r\n 之后），
    // m_read_idx 是已读入缓冲区的末尾。两者之差就是"偷跑"的 body 字节。
    long body_in_buf = m_read_idx - m_checked_idx;
    if (body_in_buf > 0)
    {
        // 不能超过剩余要收的字节数（防止重复发送的 chunk 导致溢出）
        long to_write = (body_in_buf > remaining) ? remaining : body_in_buf;

        ssize_t written = mgr->write_chunk(
            m_upload_filename,
            m_read_buf + m_checked_idx,   // 数据起始位置
            (size_t)to_write,
            (size_t)offset);

        if (written < 0)
        {
            snprintf(m_upload_response_body, sizeof(m_upload_response_body),
                     "{\"status\":\"error\",\"msg\":\"写入临时文件失败\"}");
            return false;
        }

        remaining -= written;
        offset    += written;
    }

    // ============================================================
    // 步骤 2：循环 recv 直到收够所有分块数据
    // ============================================================
    if (remaining > 0)
    {
        // ---- 设置 30 秒接收超时 ----
        // 非阻塞 socket + SO_RCVTIMEO：recv 在内核没有数据时最多等 30 秒，
        // 超时后返回 -1，errno = EAGAIN。这比纯粹的忙等循环优雅，也避免了
        // 一个卡死的客户端永远占用工作线程。
        struct timeval tv = {30, 0};
        setsockopt(m_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // 栈上 8KB 缓冲区：足够大以减少系统调用次数，又足够小不浪费栈空间
        char buf[8192];

        while (remaining > 0)
        {
            // 每次最多收 8KB，或是剩余字节数（如果剩余不足 8KB）
            size_t to_read = (remaining > (long)sizeof(buf))
                             ? sizeof(buf) : (size_t)remaining;

            ssize_t n = recv(m_sockfd, buf, to_read, 0);

            if (n <= 0)
            {
                // n == 0：客户端主动关闭连接（发送方断开）
                // n < 0：出错或超时（errno == EAGAIN/EWOULDBLOCK 表示超时）
                struct timeval tv_zero = {0, 0};
                setsockopt(m_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv_zero, sizeof(tv_zero));
                snprintf(m_upload_response_body, sizeof(m_upload_response_body),
                         "{\"status\":\"error\",\"msg\":\"接收数据失败或超时\"}");
                return false;
            }

            // 把收到的数据写入临时文件
            ssize_t written = mgr->write_chunk(
                m_upload_filename, buf, (size_t)n, (size_t)offset);

            if (written < 0)
            {
                struct timeval tv_zero = {0, 0};
                setsockopt(m_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv_zero, sizeof(tv_zero));
                snprintf(m_upload_response_body, sizeof(m_upload_response_body),
                         "{\"status\":\"error\",\"msg\":\"写入临时文件失败\"}");
                return false;
            }

            remaining -= written;
            offset    += written;
        }

        // ---- 恢复无超时模式（后续发送响应不需要超时） ----
        struct timeval tv_zero = {0, 0};
        setsockopt(m_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv_zero, sizeof(tv_zero));
    }

    // ============================================================
    // 步骤 3：构造成功响应
    // ============================================================
    // offset 就是当前文件已接收的总字节数（初始偏移 + 本次分块大小）
    snprintf(m_upload_response_body, sizeof(m_upload_response_body),
             "{\"status\":\"ok\",\"received\":%ld}", offset);
    return true;
}

void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address=0;
    }
}

bool http_conn::write()
{
    /*
    在生成响应报文时初始化byte_to_send，包括头部信息和文件数据大小。通过writev函数循环发送响应报文数据，根据返回值更新byte_have_send和iovec结构体的指针和长度，并判断响应报文整体是否发送成功。

若writev单次发送成功，更新byte_to_send和byte_have_send的大小，若响应报文整体发送成功,则取消mmap映射,并判断是否是长连接.
    长连接重置http类实例，注册读事件，不关闭连接，
    短连接直接关闭连接

若writev单次发送不成功，判断是否是写缓冲区满了。
    若不是因为缓冲区满了而失败，取消mmap映射，关闭连接
    若eagain则满了，更新iovec结构体的指针和长度，并注册写事件，等待下一次写事件触发（当写缓冲区从不可写变为可写，触发epollout），因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性。
    */


    int temp;
    /*
    temp > 0：成功发送temp字节；
    temp = 0：客户端主动关闭连接（少见，一般伴随 EPIPE）；
    temp < 0：系统调用失败，需通过errno判断失败类型。 
    */
    
    //若要发送的数据长度为0
    //表示响应报文为空，一般不会出现这种情况
    if(bytes_to_send==0)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN,m_TRIGMode);
        init();
        return true;
        //告诉上层（比如 epoll 事件循环）：当前连接是否还能继续使用（true = 可复用 / 暂等重试；false = 需关闭连接）。
    }

    while(1)
    {
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp=writev(m_sockfd,m_iv,m_iv_count);

        //若没有正常发送
        if(temp<0)
        {
            //判断缓冲区是否满了
            if(errno==EAGAIN)
            {
                //重新注册写事件
                modfd(m_epollfd,m_sockfd,EPOLLOUT,m_TRIGMode);
                return true;
            }
            unmap();
            return false;
            /*
            若writev失败（temp<0）：
            是EAGAIN（缓冲区满）：设为写事件，返回 true（等重试）；
            非EAGAIN（致命错误）：释放 mmap，返回 false（关连接）；
            */
        }

        //正常发送，temp为发送的字节数
        bytes_have_send+=temp;//更新已发送字节
        bytes_to_send-=temp;//更新将要发送字节

        //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        //m_iv[0].iov_len等价于m_write_idx
        if(bytes_have_send>=m_iv[0].iov_len)
        {
            m_iv[0].iov_len=0;
            m_iv[1].iov_base=m_file_address+(bytes_have_send - m_write_idx);//可能发送为完m_write_buf接着又发送了一部分m_iv[1]的内容
            m_iv[1].iov_len=bytes_to_send;
        }
        else//继续发送第一个iovec头部信息的数据
        {
            m_iv[0].iov_base=m_write_buf+bytes_have_send;
            m_iv[0].iov_len-=bytes_have_send;
        }
        
        //判断条件，数据已全部发送完
        if(bytes_to_send<=0)
        {
            unmap();
            //在epoll树上重置EPOLLONESHOT事件
            modfd(m_epollfd,m_sockfd,EPOLLIN,m_TRIGMode);

            if(m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char *format,...)
{
    //如果写入内容超出m_write_buf大小则报错
    if(m_write_idx>=WRITE_BUFFER_SIZE)
        return false;

    //定义可变参数列表
    va_list arg_list;

    //将变量arg_list初始化为传入参数
    va_start(arg_list,format);

    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    //最多写入 WRITE_BUFFER_SIZE - 1 - m_write_idx 字节（保留一个字节给 \0）。
    //返回值 len 是想写入的字符数（不包括 \0）。
    int len = vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-1-m_write_idx,format,arg_list);

    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if(len>WRITE_BUFFER_SIZE-1-m_write_idx)
    {
        va_end(arg_list);
        return false;
    }

    //更新m_write_idx位置
    m_write_idx+=len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

//添加状态行
bool http_conn::add_status_line(int status,const char *title)
{
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}
//添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() && add_blank_line();
}
//添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n",content_len);
}
//添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n","text/html");
}
//添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n",(m_linger==true) ? "keep-alive" : "close");
}
//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
//添加文本content,这个一般是用来追加error_500_form这些的，而不是html文件，html由write函数与请求行以及头部一起发送
bool http_conn::add_content(const char *content)
{
    return add_response("%s",content);
}

bool http_conn::process_write(HTTP_CODE ret)
{
/*
成功：iovec = [响应头(m_write_buf), 响应体(mmap)] → 头 + 体分离，效率高；
失败：iovec = [响应头+错误文本(m_write_buf)] → 无独立体，全在头部缓冲区；
*/
    switch(ret)
    {
        //内部错误，500
        case INTERNAL_ERROR:
        {
            //状态行
            add_status_line(500,error_500_title);
            //消息报头
            add_headers(strlen(error_500_title));//为什么这里是title而不是from！！！！！！
            if(!add_content(error_500_title))
                return false;
            break;
        }
        //请求资源不存在，404
        case NO_RESOURCE:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        //报文语法有误，400
        case BAD_REQUEST:
        {
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form))
                return false;
            break;
        }
        //资源没有访问权限，403
        case FORBIDDEN_REQUEST:
        {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
        }
        //文件存在，200
        case FILE_REQUEST:
        {
            add_status_line(200,ok_200_title);
            //如果请求的资源存在
            if(m_file_stat.st_size!=0)
            {
                add_headers(m_file_stat.st_size);
                //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
                m_iv[0].iov_base=m_write_buf;
                m_iv[0].iov_len=m_write_idx;
                //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
                m_iv[1].iov_base=m_file_address;
                m_iv[1].iov_len=m_file_stat.st_size;
                m_iv_count=2;
                //发送的全部数据为响应报文头部信息和文件大小
                bytes_to_send=m_write_idx+m_file_stat.st_size;
                return true;
            }
            //如果请求的资源大小为0，则返回空白html文件
            else
            {
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
            break;
        }
        // ---------- 上传功能：JSON 响应 ----------
        // /upload/init 和 /upload/complete 成功后返回此状态。
        // JSON 响应体已经在 m_upload_response_body 中构造好了，
        // 这里只需把它包装成 HTTP 响应（状态行 + 必要的头部 + JSON body）。
        case UPLOAD_RESPONSE:
        {
            add_status_line(200, ok_200_title);
            // 手动构造 Content-Type: application/json（而非默认的 text/html）
            add_response("Content-Type: application/json\r\n");
            add_content_length(strlen(m_upload_response_body));
            add_linger();
            add_blank_line();
            if (!add_content(m_upload_response_body))
                return false;
            break;
        }
        default:
            return false;
    }
    //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区。因为没有必要返回html文件
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();

    //NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if(read_ret==NO_REQUEST)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN,m_TRIGMode);
        return;
    }

    // ---------- 上传功能：流式接收 /upload/chunk 的二进制 body ----------
    // process_read() 返回 UPLOAD_STREAMING 表示 headers 已解析完毕，
    // 这是一个 /upload/chunk 请求，body 是二进制分块数据，不能放在 m_read_buf。
    // 调用 receive_file_chunk() 直接从 socket 读到磁盘，绕过读缓冲区。
    if (read_ret == UPLOAD_STREAMING)
    {
        if (receive_file_chunk())
            read_ret = UPLOAD_RESPONSE;    // 接收成功 → 返回 JSON 确认
        else
            read_ret = INTERNAL_ERROR;     // 接收失败 → 用 m_upload_response_body 返回错误
    }

    //调用process_write完成报文响应
    bool write_ret = process_write(read_ret);
    if(!write_ret)
        close_conn();
    modfd(m_epollfd,m_sockfd,EPOLLOUT,m_TRIGMode);
}
