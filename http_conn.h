#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <iostream>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <cstdio>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>

class http_conn{
public:
    static int m_epollfd; //所有socket上的事件都被注册到同一个epoll上
    static int m_user_count; //统计用户数量
    static const int FILENAME_LEN = 200;        // url文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;   // 读缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;  // 写缓冲区的大小

    // 用于解析请求报文的 请求方法、主、从状态机、响应结果
    // HTTP请求方法，这里只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    
    // 有限状态机，主从状态机
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE : 当前正在分析请求行
        CHECK_STATE_HEADER : 当前正在分析头部字段
        CHECK_STATE_CONTENT : 当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
        
    /*
        从状态机的三种可能状态，即行的读取状态，分别表示 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    */
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
    
    // 响应结果
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, 
        FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };


public:
    http_conn(){}
    ~http_conn(){}

public:
    void init(int sockfd, const sockaddr_in & addr); // 初始化新接受的连接
    void close_conn(); // 关闭连接
    bool read(); // 主线程非阻塞读客户端数据
    bool write(); // 主线程将相应非阻塞写入socket
    void process(); // 子线程处理客户端请求，http请求的入口函数。解析http请求报文，找到对应资源，等主线程可以写了之后把资源写回去
    
private:    
    void init(); // 初始化其他信息
    HTTP_CODE process_read(); // 解析http请求， 请求行，请求头，请求体
    bool process_write(HTTP_CODE ret); // 填充http响应

    // 下面这一组函数被process_read调用以分析HTTP请求
    HTTP_CODE parse_req_line(char * text); //解析请求首行
    HTTP_CODE parse_headers(char * text); // 解析请求头
    HTTP_CODE parse_content(char * text); // 解析请求体
    HTTP_CODE do_request(); // 具体的处理
    LINE_STATUS parse_line(); // 解析某一行得到的读取状态， 从状态机 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整

    char* get_line() {return m_read_buf + m_start_line;}

    // 这一组函数被process_write调用以填充HTTP应答。
    void unmap();
    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_content_type();
    bool add_status_line( int status, const char* title );
    bool add_headers( int content_length );
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line();

private:
    
    int m_sockfd; //该http连接的socket
    sockaddr_in m_address; // 客户端的socket地址
    // 用于主线程读客户数据
    char m_read_buf[READ_BUFFER_SIZE]; // 读缓冲区
    int m_read_idx; // 标识读缓冲区中已读入的客户端数据的最后一位的下一个位置

    // 用于子线程解析请求报文
    int m_check_idx; // 当前正在分析的字符在读缓冲区的位置
    int m_start_line; // 当前正在解析的行的起始位置

    CHECK_STATE m_check_state; // 主状态机当前所处状态， 三种

    METHOD m_method; // 请求方法

    char m_real_file[ FILENAME_LEN ]; // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    char* m_url; // 客户请求的目标文件的文件名
    char* m_version; // HTTP协议版本号，我们仅支持HTTP1.1
    char* m_host; // 主机名
    int m_content_length; // HTTP请求的消息总长度
    bool m_linger; // 判断HTTP请求是否要保持连接

    struct stat m_file_stat;  // 目标文件的状态。判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    char* m_file_address; // 请求的目标文件被mmap到内存中的起始位置（内存映射）
    
    char m_write_buf[WRITE_BUFFER_SIZE]; // 写缓冲区
    int m_write_idx; // 写缓冲区中待发送的字节数(已写入数据的最后一位的下一个位置)

    /*我们将采用writev来执行写操作，所以定义下面两个成员，
        iovec 结构体数组指定了要写入的缓冲区和每个缓冲区的长度，m_iv_count表示被写内存块的数量*/
    struct iovec m_iv[2];                   
    int m_iv_count;

};



#endif