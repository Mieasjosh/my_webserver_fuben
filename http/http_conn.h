#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>


#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"
#include "../upload/upload_handler.h"  // 上传功能：UploadManager 单例、UploadMeta 元数据

class http_conn
{
public:
    //设置读取文件的名称m_real_file大小
    static const int FILENAME_LEN=200;
    //设置读缓冲区m_read_buf大小
    static const int READ_BUFFER_SIZE=2048;
    //设置写缓冲区m_read_buf大小
    static const int WRITE_BUFFER_SIZE=1024;

    //报文的请求方法，本项目只用到GET和POST
    enum METHOD{GET=0,POST,HEAD,PUT,DELETE,TRACE,OPTIONS,CONNECT,PATH};

    //主状态机的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE=0,//为什么要写等于0
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };

    //报文解析的结果
    enum HTTP_CODE
    {
        NO_REQUEST,//请求不完整，需要继续读取请求报文数据
        GET_REQUEST,//获得了完整的HTTP请求
        BAD_REQUEST,//HTTP请求报文有语法错误
        NO_RESOURCE,//请求资源不存在
        FORBIDDEN_REQUEST,//FORBIDDEN_REQUEST
        FILE_REQUEST,//请求资源可以正常访问
        INTERNAL_ERROR,//服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
        CLOSED_CONNECTION,

        // ---------- 上传功能新增 ----------
        // UPLOAD_STREAMING：/upload/chunk 请求，headers 解析完毕，body 是二进制分块，
        //                   不能缓存在 m_read_buf（太大），需要直接流式写入磁盘
        UPLOAD_STREAMING,
        // UPLOAD_RESPONSE：/upload/init 或 /upload/complete 已处理完毕，
        //                  JSON 响应体已写入 m_upload_response_body，等待发送
        UPLOAD_RESPONSE
    };

    //从状态机的状态
    enum LINE_STATUS
    {
        LINE_OK=0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //初始化套接字地址，函数内部会调用私有方法init
    void init(int sockfd,const sockaddr_in &addr,char *root,int TRIGMode,int close_log,string user,string passwd,string sqlname);//这里和推文的不一样，为什么要新加这些东西？？？
    //关闭http连接
    void close_conn(bool read_close = true);
    void process();
    //读取浏览器端发来的全部数据
    bool read_once();
    //响应报文写入函数
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    //同步线程初始化数据库读取表
    void initmysql_result(connection_pool *connPool);

    //这里也和推文不一样，少了void initresultFile(connection_pool *connPool);CGI使用线程池初始化数据库表，多了两个变量
    int timer_flag;// 标记“需触发定时器关闭连接”
    int improv;// 标记“处理完成”

private:
    void init();
    //从m_read_buf读取，并处理请求报文
    HTTP_CODE process_read();
    //向m_write_buf写入响应报文数据
    bool process_write(HTTP_CODE ret);
     //主状态机解析报文中的请求行数据
    HTTP_CODE parse_request_line(char *text);
    //主状态机解析报文中的请求头数据
    HTTP_CODE parse_headers(char *text);
    //主状态机解析报文中的请求内容
    HTTP_CODE parse_content(char *text);
    //生成响应报文
    HTTP_CODE do_request();

    // ---------- 上传功能新增方法 ----------

    // 流式接收 /upload/chunk 的二进制 body：直接从 socket recv → pwrite 写磁盘
    // 不经过 m_read_buf（因为分块可能 > 2KB），读取完毕后构造 JSON 响应到 m_upload_response_body
    bool receive_file_chunk();

    // 简易 JSON 解析：从 JSON 字符串中提取指定 key 的字符串值
    // 例：{"filename":"test.mp4"} → json_get_string(json, "filename") → "test.mp4"
    // 会在源字符串中直接写 '\0' 截断，返回指向源字符串内部的指针；找不到返回 NULL
    static char *json_get_string(char *json, const char *key);

    // 简易 JSON 解析：从 JSON 字符串中提取指定 key 的数值
    // 例：{"totalSize":1024} → json_get_long(json, "totalSize") → 1024
    // 找不到返回 -1
    static long json_get_long(char *json, const char *key);

    //m_start_line是已经解析的字符个数
    //get_line用于将指针向后偏移，指向未处理的字符   总是忘记这两个变量代表的含义！！！
    char *get_line() {return m_read_buf+m_start_line;};

    //从状态机读取一行，分析是请求报文的哪一部分
    LINE_STATUS parse_line();

    void unmap();

    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用    目前还不知道如何调用！！！！
    bool add_response(const char *format,...);
    bool add_content(const char *content);
    bool add_status_line(int status,const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;//我记得定时器部分有一个u_epollfd,这两个的作用分别是什么？有什么区别？？？？
    static int m_user_count;
    MYSQL *mysql;
    int m_state; //读为0，写为1

private:
    int m_sockfd;//目前猜测不是用来监听的，而是用来通信的
    sockaddr_in m_address;

    //存储读取的请求报文数据
    char m_read_buf[READ_BUFFER_SIZE]; 

    //缓冲区中m_read_buf中数据的最后一个字节的下一个位置
    long m_read_idx;
    //m_checked_idx表示从状态机在m_read_buf中读取的位置
    long m_checked_idx;
    //m_read_buf中已经解析的字符个数,m_start_line是行在buffer中的起始位置，将该位置后面的数据赋给text
    int m_start_line;

    //存储发出的响应报文数据
    char m_write_buf[WRITE_BUFFER_SIZE];
    //指示buffer中的长度
    int m_write_idx;

    //主状态机的状态
    CHECK_STATE m_check_state;
    //请求方法
    METHOD m_method;

    //以下为解析请求报文中对应的6个变量
    //存储读取文件的名称
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    long m_content_length;
    bool m_linger;//true表示长连接，通过解析报文头部获取

    char *m_file_address;//读取服务器上的文件地址
    struct stat m_file_stat;//stat是什么？？？？有什么用？
    struct iovec m_iv[2];//io向量机制iovec
    int m_iv_count;
    int cgi;//是否启用POST
    char *m_string;//存储请求头的content部分,但可能没有存储完整数据，这也是parse_content存在的意义
    int bytes_to_send;//剩余发送字节数
    int bytes_have_send;//已发送字节数
    char *doc_root; //存网站根目录

    map<string,string> m_users;
    int m_TRIGMode;//ET是1，LT是0
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];

    // ===================== 上传功能新增成员 =====================

    // m_upload_streaming：标记当前请求是否需要流式接收 body
    //   true → /upload/chunk 请求，body 不从 m_read_buf 读，直接 recv → pwrite 到磁盘
    bool m_upload_streaming;

    // m_upload_content_len：/upload/chunk 请求的 Content-Length 值
    //   等于当前分块的大小（字节），流式接收循环的总字节数
    long m_upload_content_len;

    // m_upload_chunk_offset：/upload/chunk 请求中 X-Offset 头部的值
    //   表示这个分块在文件中的起始偏移量，传给 pwrite 定位写
    long m_upload_chunk_offset;

    // m_upload_filename[256]：从 X-Filename 头部提取的文件名
    //   客户端在每个上传请求中都带这个头部，服务器用它查找对应的 UploadMeta
    char m_upload_filename[256];

    // m_upload_response_body[1024]：上传 API 的 JSON 响应体
    //   如 {"status":"ok","received":1048576}，复用 WRITE_BUFFER_SIZE 的大小
    char m_upload_response_body[1024];

};


#endif
