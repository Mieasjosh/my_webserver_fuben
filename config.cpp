#include "config.h"

Config::Config()//为所有配置参数设置默认值，即使用户不传入命令行参数，服务器也能以默认配置启动。
{
    //端口号,默认9006
    PORT=9006;

    //日志写入方式，默认同步
    LOGWrite=0;

     //触发组合模式,默认listenfd LT + connfd LT
     TRIGMode=0;

     //listenfd触发模式，默认LT
     LISTENTrigmode=0;

     //connfd触发模式，默认LT
     CONNTrigmode = 0;

     //优雅关闭链接，默认不使用
     OPT_LINGER=0;

     //数据库连接池数量,默认8
     sql_num=8;

     //线程池内的线程数量,默认8
     thread_num=8;

     //关闭日志,默认不关闭
     close_log=0;

     //并发模型,默认是proactor
     actor_model=0;
}

void Config::parse_arg(int argc,char *argv[])
{
    int opt;
    const char *str="p:l:m:o:s:t:c:a:";

    //getopt（Linux 系统函数），用于解析带短选项的命令行参数（如 -p 8080、-l 1）
    //optarg：如果选项需要参数，optarg 指向该参数的字符串。
    //str = "p:l:m:o:s:t:c:a:"：每个字母后加 : 表示该选项需要跟参数（如 -p 后必须跟端口号）
    //toi：将字符串参数转为整数（配置参数均为 int 类型）
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
            case 'p':
            {
                PORT=atoi(optarg);
                break;
            }
            case 'l':
            {
            LOGWrite = atoi(optarg);
            break;
            }
        case 'm':
            {
            TRIGMode = atoi(optarg);
            break;
            }
        case 'o':
            {
            OPT_LINGER = atoi(optarg);
            break;
            }
        case 's':
            {
            sql_num = atoi(optarg);
            break;
            }
        case 't':
            {
            thread_num = atoi(optarg);
            break;
            }
        case 'c':
            {
            close_log = atoi(optarg);
            break;
            }
        case 'a':
            {
            actor_model = atoi(optarg);
            break;
            }
        default:
            break;
        }
    }
}