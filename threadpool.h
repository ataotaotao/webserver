#ifndef THREADPOLL_H
#define THREADPOLL_H



#include <iostream>
#include <pthread.h>
#include <list>
#include "locker.h"
#include <exception>
#include <cstdio>

using namespace std;

// 使用模板类，线程池，为了代码的复用,参数T为任务类
template< typename T >
class threadpool {
public:
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T * request);

private:
    static void * worker(void * arg);
    void run();
    // 线程数量
    int m_thread_number;

    // 线程池数组，大小为m_thread_number;
    pthread_t * m_threads;

    // 请求队列中最多允许的等待的数量
    int m_max_request;

    //请求队列
    std::list<T*> m_workqueue;

    // 互斥锁
    locker m_queuelocker;

    // 信号量用来判断是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程
    bool m_stop;

};

template< typename T >
threadpool<T>::threadpool(int thread_number, int max_requests) : 
m_thread_number(thread_number), m_threads(NULL), 
m_max_request(max_requests), m_stop(false)
{
    if (thread_number <= 0  || max_requests <= 0) {
        throw std::exception();
    }
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) {
        throw std::exception();
    }

    // 创建线程之后设置为线程detach
    for (int i = 0; i < thread_number; ++ i) {
        printf("create the %dth thread \n", i);
        if (pthread_create(&m_threads[i], NULL, worker, (void *)this ) != 0) {
            delete [] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i])){
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template< typename T >
threadpool<T> :: ~threadpool(){
    delete m_threads;
    m_stop = true;
}

template< typename T >
bool threadpool<T>::append(T * request) {
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_request) {
        m_queuelocker.unlock();
        return false;
    }
    // 可以处理这个问题
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post(); // 信号量要增加
    return true;
}

template< typename T >
void * threadpool<T>::worker(void * arg) {
    // 静态无法访问当前的对象，可以将this传递过来
    threadpool * pool = (threadpool *)arg;
    pool->run();
    return pool;
}
template< typename T >
void  threadpool<T>::run() {
    while(!m_stop) {
        m_queuestat.wait();
        // 有数据进来了
        m_queuelocker.lock();
        
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        T * request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request) {
            continue;
        }
        // 处理任务的时候需要处理process，即写出process类
        request->process();
    }
}






# endif