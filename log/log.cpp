#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log()
{
    m_count=0;
    m_is_async=false;//默认同步
}

Log::~Log()
{
    if(m_fp!=NULL)
    {
        fclose(m_fp);
    }
}

//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name,int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    //如果设置了max_queue_size,则设置为异步
    if(max_queue_size>=1)
    {
        m_is_async=true;
        //创建并设置阻塞队列长度
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid,NULL,flush_log_thread,NULL);
    }

    //输出内容的长度
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);

    //日志的最大行数
    m_split_lines = split_lines;

    time_t t=time(NULL);//传入 NULL 表示「获取当前时间的 Unix 时间戳」

    //把 time_t 类型的 Unix 时间戳，转换成「本地时区」的 tm 结构体指针,localtime 返回的指针指向静态内存（全局共享），如果后续再调用 localtime，这个指针的值会被覆盖，所以代码里要拷贝一份到 my_tm
    struct tm *sys_tm=localtime(&t);
    struct tm my_tm=*sys_tm;
    /*
    struct tm
    存储「本地时间」的结构体，包含年、月、日、时、分、秒等字段（人类可读的时间），核心字段：
    - tm_year：年份（需 +1900，比如 tm_year=124 → 2024）；
    - tm_mon：月份（0-11，需 +1，比如 tm_mon=4 → 5 月）；
    - tm_mday：日期（1-31）；
    - tm_hour/tm_min/tm_sec：时分秒。
    */

    //从后往前找到第一个/的位置
    const char *p=strrchr(file_name,'/');
    char log_full_name[300]={0};

    //若输入的文件名没有/，则直接将时间+文件名作为日志名,相当于自定义文件名
    if(p==NULL)
    {
        snprintf(log_full_name,299,"%d_%02d_%02d_%s",my_tm.tm_year+1900,my_tm.tm_mon+1,my_tm.tm_mday,file_name);
    }
    else
    {
        //将/的位置向后移动一个位置，然后复制到logname中
        //p - file_name + 1是文件所在路径文件夹的长度
        //dirname相当于./
        strcpy(log_name,p+1);// 把'/'后面的文件名部分拷贝到log_name（比如 "server.log"）
        strncpy(dir_name,file_name,p-file_name+1); // 把'/'前面的路径部分拷贝到dir_name（比如 "./log"）
        //结合当前时间生成log_full_name（带日期） 拼接：目录 + 纯文件名（去掉后缀） + _ + 年月日 + .log
        snprintf(log_full_name, 299, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today=my_tm.tm_mday;

    // 步骤4：打开日志文件,"a"是追加模式
    m_fp=fopen(log_full_name,"a");
    if(m_fp==NULL)
        return false;

    return true;
}

void Log::write_log(int level,const char *format,...)
{
    struct timeval now={0,0};
    gettimeofday(&now,NULL);//把当前系统时间写入传入的 timeval 结构体
    time_t t=now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16]={0};

    //日志分级
    switch (level)
    {
        
        case 0:
            strcpy(s, "[debug]:");
            break;
        case 1:
            strcpy(s, "[info]:");
            break;
        case 2:
            strcpy(s, "[warn]:");
            break;
        case 3:
            strcpy(s, "[erro]:");
            break;
        default:
            strcpy(s, "[info]:");
            break;
    }

    //写入一个log，对比m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++;

    //日志不是今天或写入的日志行数是最大行的倍数
    if(m_today!=my_tm.tm_mday || m_count%m_split_lines==0)
    {
        char new_log[256]={0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16]={0};

        //格式化日志名中的时间部分
        snprintf(tail,16,"%d_%02d_%02d_",my_tm.tm_year+1900,my_tm.tm_mon+1,my_tm.tm_mday);

        //如果是时间不是今天,则创建今天的日志，更新m_today和m_count
        if(m_today != my_tm.tm_mday)
        {
            snprintf(new_log,299,"%s%s%s",dir_name,tail,log_name);
            m_today=my_tm.tm_mday;
            m_count=0;
        }
        else
        {
            //超过了最大行，在之前的日志名基础上加后缀, m_count/m_split_lines
            snprintf(new_log,299,"%s%s%s.%lld",dir_name,tail,log_name,m_count/m_split_lines);
        }
        m_fp=fopen(new_log,"a");
    }
    m_mutex.unlock();
    va_list valist;
    va_start(valist,format);

    string log_str;
    m_mutex.lock();

    //写入内容格式：时间 + 内容
    //时间格式化，snprintf成功返回写字符的总数，其中不包括结尾的null字符
    int n=snprintf(m_buf,48,"%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec,now.tv_usec,s);

    //内容格式化，用于向字符串中打印数据、数据格式用户自定义，返回写入到字符数组str中的字符个数(不包含终止符)
    int m=vsnprintf(m_buf+n,m_log_buf_size-n-1,format,valist);// 留 1 个位置给\0
    m_buf[n+m]='\n';
    m_buf[n+m+1]='\0';//vsnprintf 本身会在写入的内容末尾加\0，但我们手动加了\n，所以需要在\n后面补\0
    log_str=m_buf;

    m_mutex.unlock();

    //若m_is_async为true表示异步，默认为同步,若异步,则将日志信息加入阻塞队列,同步则加锁向文件中写
    if(m_is_async && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(),m_fp);
        m_mutex.unlock();
    }

    va_end(valist);
}

void Log::flush()
{
    m_mutex.lock();
    fflush(m_fp);
    m_mutex.unlock();
}