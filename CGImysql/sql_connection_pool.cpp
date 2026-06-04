#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
    m_CurConn=0;
    m_FreeConn=0;
}

connection_pool *connection_pool::GetInstance()
{
    static connection_pool connPool;
    return &connPool;
}

void connection_pool::init(string url,string user,string passord,string databasename,int port,int maxconn,int close_log)
{
    m_url=url;
    m_User=user;
    m_Passord=passord;
    m_DatebaseName=databasename;
    m_Port=port;
    m_MaxConn=maxconn;
    m_close_log = close_log;

    for(int i=0;i<maxconn;i++)
    {
        MYSQL *con=NULL;
        con=mysql_init(con);
        
        if(con==NULL)
        {
            LOG_ERROR("MySQL Error");
			exit(1);
        }

        con=mysql_real_connect(con,url.c_str(),user.c_str(),passord.c_str(),databasename.c_str(),port,NULL,0);

        if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
        connlist.push_back(con);
        ++m_FreeConn;
    }
    reserve=sem(m_FreeConn);
    m_MaxConn=m_FreeConn;
}

MYSQL *connection_pool::GetConnection()
{
    MYSQL *con=NULL;

    if(0==connlist.size())
    {
        return NULL;
    }

    reserve.wait();
    lock.lock();

    con=connlist.front();
    connlist.pop_front();
    
    --m_FreeConn;
    ++m_CurConn;

    lock.unlock();
    return con;
}

bool connection_pool::ReleaseConnection(MYSQL *con)
{
    if(NULL==con)
    return false;

    lock.lock();
    connlist.push_back(con);
    ++m_FreeConn;
    --m_CurConn;
    lock.unlock();
    reserve.post();
    return true;
}

void connection_pool::DestoryPool()
{
    lock.lock();
    if(connlist.size()>0)
    {
        list<MYSQL *>::iterator i;
        for(i=connlist.begin();i!=connlist.end();i++)
        {
            MYSQL *con=*i;
            mysql_close(con);
        }
        m_CurConn=0;
        m_FreeConn=0;
        connlist.clear();
    }
    lock.unlock();
}

int connection_pool::GetFreeconn()
{
    return m_FreeConn;
}

connection_pool::~connection_pool()
{
    DestoryPool();
}

connectionRAII::connectionRAII(MYSQL **SQL,connection_pool *connPool)
{
    *SQL=connPool->GetConnection();
    conRAII=*SQL;
    poolRAII=connPool;
}

connectionRAII::~connectionRAII()
{
    poolRAII->ReleaseConnection(conRAII);
}