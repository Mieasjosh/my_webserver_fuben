#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    threadpool(int m_actor_model,connection_pool *connPool,int thread_num=8,int max_requests=10000);
    ~threadpool();
    bool append(T *request,int state);
    bool append_p(T *request);

private:
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;
    int m_max_requests;
    pthread_t *m_threads;
    std::list<T *> m_workqueue;
    locker m_queueloker;
    sem m_queuestat;
    connection_pool *m_connPool;  //数据库
    int m_actor_model;          //模型切换

};

template <typename T>
threadpool<T>::threadpool(int actor_model,connection_pool *connPool,int thread_number,int max_requests):m_actor_model(actor_model),m_thread_number(thread_number),m_max_requests(max_requests),m_threads(NULL),m_connPool(connPool)
{
    if(thread_number<=0 || max_requests<=0)
        throw exception();
    m_threads=new pthread_t(m_thread_number);
    if(!m_threads) throw exception();
    for(int i=0;i<m_thread_number;i++)
    {
        if(pthread_create(m_threads+i,NULL,worker,this)!=0)
        {
            delete[] m_threads;
            throw exception();
        }
        if(pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

template <typename T>
bool threadpool<T>::append(T *request,int state)
{
    m_queueloker.lock();
    if(m_workqueue.size()>=m_max_requests)
    {
        m_queueloker.unlock();
        return false;
    }
    request->m_state=state;
    m_workqueue.push_back(request);
    m_queueloker.unlock();
    m_queuestat.post();
    return true;
}

template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queueloker.lock();
    if(m_workqueue.size()>=m_max_requests)
    {
        m_queueloker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queueloker.unlock();
    m_queuestat.post();
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg)//这个函数是子线程还是主线程调用的？!!!!
{
    threadpool *pool=(threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run()
{
    while(true)
    {
        m_queuestat.wait();
        m_queueloker.lock();
        if(m_workqueue.empty())
        {
            m_queueloker.unlock();
            continue;
        }

        T *request=m_workqueue.front();
        m_workqueue.pop_front();
        m_queueloker.unlock();
        if(!request)
            continue;
        if(1==m_actor_model)//reactor模式
        {
            // 执行读IO：读取客户端的HTTP请求数据
            if(0==request->m_state)//读状态,m_state=0
            {
                if(request->read_once())//读成功
                {
                    request->improv=1;//标记读成功，需要处理
                    connectionRAII mysqlcon(&request->mysql,m_connPool);
                    request->process();// 处理HTTP请求（解析、查库、构造响应）
                }
                else{//读失败（比如客户端断开、超时）
                    request->improv=1;// 标记“处理完成”
                    request->timer_flag=1;// 标记“需触发定时器关闭连接”
                }
            }
            else//state=1,写状态
            {
                // 执行写IO：把HTTP响应写回客户端
                if(request->write())
                {
                    request->improv=1;
                }
                else{//写失败
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else// Proactor 模式（异步IO）
        {
            
            //为什么这里不考虑把数据写回客户端？？？？？？？？？？？？？？？？？？？？？？

            // Proactor模式下，内核已完成IO（读/写），直接处理结果即可
            connectionRAII mysqlcon(&request->mysql,m_connPool);
            request->process();// 处理HTTP请求（解析、查库、构造响应）
        }
    }
}

#endif