#ifndef LOCKER_H
#define LOCKER_H

// 线程同步机制 互斥锁、条件变量、信号量 
// 函数的返回值为0表示成功
#include <exception>
#include<pthread.h>
#include<semaphore.h>
using namespace std;

//互斥锁类 
class locker{
public:
    locker(){
        if(pthread_mutex_init(&m_mutex, NULL) != 0){ // 出错了
            throw exception();
        }
    }
    ~locker(){
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock(){
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock(){
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t * get(){ //获取互斥量成员
        return &m_mutex;
    }
private:
    pthread_mutex_t m_mutex;
};

// 条件变量类 条件变量不是锁，条件满足后阻塞线程或解除阻塞，配合互斥量使用
//判断有无数据，无数据就停在那，有数据继续
/*
一个线程在等待某个条件成立时会调用 pthread_cond_wait()，但在等待期间，其他线程可能会修改条件，并可能需要互斥锁来保护这些条件的访问。 
因此，pthread_cond_wait() 在等待期间（阻塞时）会对互斥锁进行解锁，以允许其他线程修改条件，当不阻塞的时候，继续向下执行，会重新加锁。
*/
class cond{
public:
    cond(){
        if(pthread_cond_init(&m_cond, NULL) != 0){
            throw exception();
        }
    }
    ~cond(){
        pthread_cond_destroy(&m_cond);
    }
    //在等待期间（阻塞时）会对互斥锁进行解锁以允许其他线程修改条件，当不阻塞的时候，继续向下执行，重新加锁。
    bool wait(pthread_mutex_t * mutex){
        return pthread_cond_wait(&m_cond, mutex) == 0;
    }
    bool timewait(pthread_mutex_t * mutex, struct timespec t){
        return pthread_cond_timedwait(&m_cond, mutex, &t) == 0; // 超过了这个时间条件还没有满足，函数会返回ETIMEDOUT
    }
    // 唤醒一个
    bool signal(pthread_mutex_t * mutex){
        return pthread_cond_signal(&m_cond) == 0;
    }
    //唤醒所有线程
    bool broadcast(pthread_mutex_t * mutex){
        return pthread_cond_broadcast(&m_cond) == 0;
    }
private:
    pthread_cond_t m_cond;
};

// 信号量类
class sem{
public:
    sem(){
        if(sem_init(&m_sem, 0, 0) != 0){
            throw std::exception();
        }
    }
    sem(int num){
        if(sem_init(&m_sem, 0, num) != 0){
            throw exception();
        }
    }
    ~sem(){
        sem_destroy(&m_sem);
    }
    //等待信号量
    bool wait(){
        return sem_wait(&m_sem) == 0;
    }
    // 增加信号量 
    bool post(){
        return sem_post(&m_sem) == 0;
    }
private:
    sem_t m_sem;
};
#endif