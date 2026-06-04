#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

class connection_pool
{
public:
    MYSQL *GetConnection();//获取数据库连接
    bool ReleaseConnection(MYSQL *conn);//释放连接
    int GetFreeconn();//获取连接
    void DestoryPool();//销毁所有连接

    static connection_pool *GetInstance();

    //此函数最后还少了一个参数int close_log，目前还没写log模块，日后再加上去   --已经在6月1号加上去
    void init(string url,string user,string passord,string databasename,int port,int maxconn,int close_log);

private:
    connection_pool();
    ~connection_pool();

    int m_MaxConn;//最大连接数
    int m_CurConn;//当前已使用的连接数
    int m_FreeConn;//当前空闲的连接数
    locker lock;
    list<MYSQL *> connlist;
    sem reserve;

public:
    string m_url;  //主机地址
    int m_Port; //数据库端口号
    string m_User;
    string m_Passord;
    string m_DatebaseName;
    int m_close_log;  //日至开关

};

class connectionRAII
{
public:
    connectionRAII(MYSQL **SQL,connection_pool *connPool);
    ~connectionRAII();

private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};


#endif