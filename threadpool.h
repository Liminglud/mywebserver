#ifndef THREADPOOL_H
#define THREADPOOL_H
/*
线程池用来处理大量客户端连接和请求。在服务器启动之初创建并初始化，是静态资源。


任务池中每一个任务都有一个回调函数，执行不同操作

主子线程共享任务池资源，主线程向请求队列中添加任务，而工作线程循环从请求队列中取出任务并执行。
通过使用互斥锁和信号量来实现线程之间的同步和通信
*/
#include <list>
#include <iostream>
#include <pthread.h>
#include "locker.h"

using namespace std;
// 线程池定义为模板类
template<typename T>
class threadpool{
public:
/*thread_number是线程池中线程的数量，
    max_requests是请求队列中最多允许的请求数量*/
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T * request); // 添加任务

private:
   /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void * worker(void * arg); // 静态函数，静态函数中不能访问非静态成员
    void run(); // 启动线程池
private:
    // 线程数量
    int m_thread_number;
    // 线程池数组,大小为线程数量
    pthread_t * m_threads;
   
    // 请求队列中最多允许的请求数量
    int m_max_requests;
    // 请求队列 ->双向链表 头尾均可插入删除
    std::list<T *> m_workqueue;
   
    // 保护请求队列的互斥锁
    locker m_queuelocker;
    // 信号量判断是否有任务需要处理
    sem m_queuestat;
    
    //  是否结束线程
    bool m_stop;
};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):m_thread_number(thread_number), 
    m_max_requests(max_requests),m_stop(false), m_threads(NULL){
    if(thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    // 线程池
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads)
        throw std::exception();
    
    // 创建thread_number个分离线程
    for(int i = 0; i < thread_number; i++){
        cout << "create the " << i << "th thread." << endl; 
        // pthread_create(新线程ID存储位置,属性NULL默认, 回调函数, 传入回调函数的参数)
        // worker必须是静态函数， 所以把this（当前对象的指针）传入worker，让他可以访问成员对象、成员函数
        if(pthread_create(m_threads + i, NULL, worker, this) != 0){ 
            delete [] m_threads;
            throw std::exception();
        }

        if(pthread_detach(m_threads[i]) != 0){
            delete[] m_threads;
            throw exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool(){
    delete[] m_threads;
    m_stop = true; //是否结束线程，默认true
}
// 请求队列中添加任务
template<typename T>
bool threadpool<T>::append(T * request){
    m_queuelocker.lock(); // 互斥锁上锁
    //请求队列线程数大于最大允许任务数，报错
    if(m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    // 请求队列中添加任务
    m_workqueue.push_back(request);
    m_queuelocker.unlock(); //解锁
    // 添加了一个信号量增加1，信号量是请求队列中的任务数
    m_queuestat.post(); 
}  

template<typename T>
void * threadpool<T>::worker(void * arg){
    threadpool * pool = (threadpool *) arg; // this类型转为对象指针
    pool->run();
    return pool;
}

// 选一个子线程做任务
template<typename T>
void threadpool<T>::run(){
    while(!m_stop){
        //信号量 -1，为0会阻塞（没有任务）
        m_queuestat.wait();
        //互斥量上锁
        m_queuelocker.lock();
        if(m_workqueue.empty()){ // 若请求队列空了
            m_queuelocker.unlock(); //互斥锁解锁，直接循环
            continue;
        }
        T * request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request){ // 任务不存在，直接循环
            continue;
        }
        // 做任务，任务类
        request->process(); // 处理http请求的入口函数，线程池中子线程调用
    }

}

#endif
