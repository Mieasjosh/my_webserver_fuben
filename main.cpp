#include "config.h"

//main.cpp 是整个服务器的入口文件，完成 “配置解析→服务器初始化→启动运行” 的全流程，是串联所有模块的 “总控中心”。
int main(int argc,char *argv[])
{
    //需要修改的数据库信息,登录名,密码,库名
    string user = "root";
    string passwd = "123456";
    string databasename = "yourdb";

    //命令行解析：初始化配置对象，解析用户传入的参数
    Config config;
    config.parse_arg(argc,argv);

    WebServer server;

    //初始化WebServer：将配置参数+数据库信息传入，完成核心参数初始化
    server.init(config.PORT,user,passwd,databasename,config.LOGWrite,config.OPT_LINGER,config.TRIGMode,config.sql_num,config.thread_num,config.close_log,config.actor_model);

    //初始化日志模块（同步/异步、开关）
    server.log_write();

    //初始化数据库连接池+加载用户表
    server.sql_pool();

    //初始化业务处理线程池
    server.thread_pool();

    //设置epoll触发模式（LT/ET）
    server.trig_mode();

    //创建监听套接字+初始化epoll+注册信号处理
    server.eventListen();

    //启动服务器主循环（epoll_wait监听+事件分发）
    server.eventLoop();

    return 0;
}