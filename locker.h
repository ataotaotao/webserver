#ifndef __LOCKER__H
#define __LOCKER__H
#include <pthread.h>
#include <exception>
#include <semaphore.h>
// 线程同步机制封装类

// 互斥锁类
class locker
{
public:
    locker(){
        if (pthread_mutex_init(&m_mutex, NULL) != 0) {
            throw std::exception();
        }
    }

    ~locker() {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock(){
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock(){
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t *get(){
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;

};

// 条件变量
class condition {
public:
    condition(){
        if(pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }
    ~condition () {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t * mutex) {
        return pthread_cond_wait(&m_cond, mutex) == 0;
    }
    bool timedwait(pthread_mutex_t * mutex, struct timespec t) {
        return pthread_cond_timedwait(&m_cond, mutex, &t) == 0;
    }
    bool signal(){
        return pthread_cond_signal(&m_cond) == 0;
    }
    bool broadcast(){
        return pthread_cond_broadcast(&m_cond) == 0;
    }
private:
    pthread_cond_t m_cond;
};


// 信号量类
class sem {
public:
    sem(){
        if (sem_init(&sems, 0, 0) != 0) {
            throw std::exception();
        }
    }
    sem(int num) {
        if (sem_init(&sems, 0, num) != 0) {
            throw std::exception();
        }
    }
    ~sem(){
        sem_destroy(&sems);
    }

    // 等待信号量
    bool wait(){
        return sem_wait(&sems) == 0;
    }
    
    // 增加信号量
    bool post(){
        
        return sem_post(&sems) == 0;
    }

private:
    sem_t sems;
};



#endif