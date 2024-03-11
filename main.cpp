#include <iostream>
#include <cstring>
#include <cstdlib>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <error.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include <signal.h>
#include "http_conn.h"

using namespace std;

#define MAX_FD 65535 // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 12000  // 监听的最大的事件数量

// 添加文件描述符到epoll中、删除fd、修改epoll上的fd
extern void addlfd(int epollfd, int fd, bool one_shot); // epoll one_shot事件
extern void removefd(int epollfd, int fd);
extern void modfd(int epollfd, int fd, int ev);

// 注册一个信号处理函数，sig 表示要注册的信号，handler 表示处理该信号的处理函数
// 声明了一个函数指针 handler，该指针指向一个函数，该函数的返回类型为 void，接受一个 int 类型的参数
void addsig(int sig, void(handler)(int)){
    // sa注册信号的参数
    struct sigaction sa; 
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler; //设置信号处理函数为参数传进来的 handler 函数
    sigfillset(&sa.sa_mask); //将信号屏蔽字设置为全集，即屏蔽所有信号。这样在处理信号时，会将其他信号临时阻塞，防止在处理一个信号时，又来了其他信号。
    assert(sigaction(sig, &sa, NULL) != -1); // 注册信号处理函数
}

// argv[]是一个字符串数组，里面包含了argc个字符串
// 在命令行参数中，argv[0] 通常是可执行文件名，所以端口号在argv[1] ./my_program 8080
int main(int argc, char* argv[]){
    if(argc <= 1){
        cout <<"按照如下格式运行：" << basename(argv[0]) << " port number" << endl;
        return 1;
    }
    //获取端口号 ./my_program 8080
    int port = atoi(argv[1]); // 将字符串转换为整数

    // 对SIGPIPE管道破裂信号（进程尝试给一个已关闭写端的管道写数据）进行处理
    addsig(SIGPIPE, SIG_IGN); // 处理方式设置为了忽略，系统不会发送 SIGPIPE 信号给程序，程序将继续执行

    //创建和初始化线程池 http连接的类
    threadpool<http_conn> * pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    } catch(...){
        exit(-1);
    }

    // 创建一个数组保存所有客户端信息, users指向首地址
    http_conn * users = new http_conn[MAX_FD];

    // 创建监听套接字
    int lfd = socket(PF_INET, SOCK_STREAM, 0); //协议族PF_INET

    //端口复用 （bind之前）
    int reuse = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    //绑定
    int ret = 0;
    struct sockaddr_in addr; 
    addr.sin_addr.s_addr = INADDR_ANY; //可以访问任意ip地址
    addr.sin_family = AF_INET; // 设置地址族为 IPv4
    addr.sin_port = htons(port);

    ret = bind(lfd, (struct sockaddr *)&addr, sizeof(addr));
    // listen
    ret = listen(lfd, 128);

    // 创建epoll对象(建议值)
    //events传出参数，内核告诉y
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(10);
    // 需要监听的fd上epoll树 http_conn
    addlfd(epollfd, lfd, false); // lfd不需要设置ontshot
    http_conn::m_epollfd = epollfd;

    while(true){
        // 循环监听等待事件发生 >0 等待事件的超时时间(ms)。 0：不阻塞， -1：阻塞直到检测到fd变化
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (num < 0 && errno != EINTR){
            cout << "epoll failure" << endl;
            break;
        }
        // 循环遍历事件数组
        for(int i = 0; i < num; i++){
            int sockfd = events[i].data.fd;
            if(sockfd == lfd){
                // 有客户端连接进来
                struct sockaddr_in client_addr;
                socklen_t client_addrlen = sizeof(client_addr);
                int cfd = accept(lfd, (struct sockaddr*)&client_addr, &client_addrlen);
                if(cfd < 0){
                    cout <<"error is " << errno << endl;
                }

                if(http_conn::m_user_count >= MAX_FD){
                    // 最大连接数满了
                    // 给客户端响应报文：服务器正忙，关闭连接
                    close(cfd);
                    continue;
                }

                // init将新连接users[cfd]初始化(cfd上epoll树) users 数组中cfd作为user索引
                // http_conn * users = new http_conn[MAX_FD];
                users[cfd].init(cfd, client_addr);
            } // 其他事件
            // 对方异常断开或错误， 关闭连接
            else if(events[i].events & (EPOLLRDHUP |EPOLLHUP | EPOLLERR)){
                users[sockfd].close_conn(); 
            } // 读事件（有客户端数据发来）
            else if(events[i].events & EPOLLIN){
                // 主线程非阻塞读，一次性全读完了
                if(users[sockfd].read()){ 
                    // 主线程把读取的数据封装成请求对象(http_conn对象)添加到请求队列中
                    pool->append(users + sockfd); // 首地址+sockfd就是users[sockfd]的地址，
                }else{
                    users[sockfd].close_conn();//失败关闭连接
                }
            } // 写事件（主线程响应）
            else if(events[i].events & EPOLLOUT){
                // 主线程非阻塞写
                if(!users[sockfd].write()){
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epollfd);
    close(lfd);
    delete [] users;
    delete pool;

    return 0;
}