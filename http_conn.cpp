#include "http_conn.h"

// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;

// 定义HTTP响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站根目录
const char *doc_root = "/home/now/myweb/resources";

// 设置文件描述符非阻塞
int setnonblocking(int cfd)
{
    int flag = fcntl(cfd, F_GETFL); // 获取cfd属性
    flag |= O_NONBLOCK;
    fcntl(cfd, F_SETFL, flag);
    return flag;
}

void addlfd(int epollfd, int fd, bool one_shot)
{
    epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLRDHUP; // 边沿触发，lfd都是LT
    if (one_shot)
    {
        ev.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    // 若ET需要设置文件描述符非阻塞，保证一次性读完。LT两个都可以
    setnonblocking(fd);
}

// 添加fd上epoll树
/*epoll one_shot事件 操作系统最多触发fd上注册的一个可读、可写或者异常事件，且只触发一次，
除非用 epoll_ctl 函数重置该文件描述符上注册的 EPOLLONESHOT 事件。*/
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLRDHUP; // 水平触发 若对方连接断开触发EPOLLRDHUP，挂起
    // ev.events = EPOLLIN | EPOLLET |EPOLLRDHUP; // 边沿触发，lfd都是LT
    if (one_shot)
    {
        ev.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    // 若ET需要设置文件描述符非阻塞，保证一次性读完。LT两个都可以
    setnonblocking(fd);
}

// 从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改epoll树上要监听的fd的事件，重置socket上的EPOLLONESHOT事件，确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev)
{ // ev 要修改的事件
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 关闭一个客户连接
void http_conn::close_conn()
{
    if (m_sockfd != -1)
    {
        removefd(m_epollfd, m_sockfd); // fd下树
        m_sockfd = -1;                 // 没用了
        m_user_count--;                // 客户数-1
    }
}

// 初始化新连接,
//  users[cfd].init(cfd, client_addr); 初始化套接字和地址，cfd上树，用户数+1
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    // 端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // fd上epoll树
    addfd(m_epollfd, m_sockfd, true); // oneshot
    m_user_count++;                   // 用户数+1
    init();
}

// 初始化其他信息
void http_conn::init()
{
    m_read_idx = 0;                          // 标识读缓冲区中已读入的客户端数据的最后一位的下一个位置
    m_check_state = CHECK_STATE_REQUESTLINE; // 主状态机当前所处的状态,初始状态为分析请求行
    m_linger = false;                        // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET; // 默认请求方式为GET
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0; // 当前正在解析的行的起始位置
    m_check_idx = 0;  // 当前正在分析的字符在读缓冲区的位置

    m_write_idx = 0;
    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

// 主线程非阻塞地循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    // 读取到的字节
    int n = 0;
    while (true)
    {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        n = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (n == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 没有数据了
                break;
            }
            return false;
        }
        else if (n == 0)
        { // 对方关闭连接
            return false;
        }
        m_read_idx += n; // 下一次读的起始位置
    }
    // cout << "read_index = " << m_read_idx << "读取到了数据:\n " << m_read_buf << endl;
    return true;
}
// 主线程非阻塞 写HTTP响应
bool http_conn::write()
{
    int tmp = 0;
    int bytes_sent = 0; // 已发送字节数
    int bytes_to_send = m_write_idx; // 写缓冲区待发送的字节数
    if(bytes_to_send == 0){
        //响应结束,重新变为等待读
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    while(1){
        // 分散写数据
        
        tmp = writev(m_sockfd, m_iv, m_iv_count); // 将数据写入到套接字文件描述符 m_sockfd 所指向的套接字中count=2或1
        if ( tmp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= tmp;
        bytes_sent += tmp;
        if ( bytes_to_send <= bytes_sent ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger) {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}

// 处理http请求的入口函数，线程池中子线程调用
void http_conn::process()
{
    // 解析http请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    { // 请求不完整
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    // 生成响应,各种错误都直接返回false
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}

// 主状态机 逐行解析http请求报文， 请求行，请求头，请求体
/*
    主状态机获取一行数据，然后根据状态做不同处理
*/
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    // 逐行解析
    //   解析到一行完整的数据 或者 解析到了请求体，也是完整的数据
   while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) 
                || ((line_status = parse_line()) == LINE_OK))
    {
        // cout << "当前check_index: " << m_check_idx;
        // 获取一行数据
        text = get_line();
        m_start_line = m_check_idx; // 当前解析行的起始位置 = 当前正在分析的字符在读缓冲区中的位置
        cout << "get 1 line: " << text << endl;

       switch (m_check_state)
        { // 主状态机当前所处的状态
          // 解析请求行
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_req_line(text);
            if (ret == BAD_REQUEST)
            { // 请求报文错误
                return BAD_REQUEST;
            }
            break;
        } // 解析请求头
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            else if (ret == GET_REQUEST)
            {
                return do_request(); // 解析具体的内容url等
            }
            break;
        } // 解析请求体
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
            {
                return do_request(); // 具体的处理
            }
            line_status = LINE_OPEN; // 行数据不完整
            break;
        }
        default:
        {
            return INTERNAL_ERROR; // 服务器内部错误
        }
        }
    }
    return NO_REQUEST;
}

// 0.解析一行得到的读取状态，windows的判断依据\r\n（回车,\r光标移到开头）
// 从状态机 1.读取到一个完整的行 2.行出错 3.行数据不完整
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    // cout << "m_check_idx = " << m_check_idx << endl;
    // cout << "m_read_idx = " << m_read_idx << endl; 
    // 遍历一行数据，check_idx检查的字符索引 < 下一次读的索引
    for (; m_check_idx < m_read_idx; ++m_check_idx)
    {
        temp = m_read_buf[m_check_idx]; // 从第0个开始解析
        if (temp == '\r')
        { // 光标移到开头
            if ((m_check_idx + 1) == m_read_idx)
            {
                return LINE_OPEN; // 行数据不完整
            }
            else if (m_read_buf[m_check_idx + 1] == '\n')
            {                                     // 遇到\r\n换行符了
                m_read_buf[m_check_idx++] = '\0'; // \r -> \0
                m_read_buf[m_check_idx++] = '\0'; // \n -> \0，且 m_check_idx 变到下一行开始位置
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if ((m_check_idx > 1) && (m_read_buf[m_check_idx - 1] == '\r'))
            {
                // 第二次读的时候从\n开始读了，前一个是\r， 第一行的
                m_read_buf[m_check_idx - 1] = '\0'; // \r 变\0
                m_read_buf[m_check_idx++] = '\0';   // \n -> \0, idx到下一行起始
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 1. 解析http请求行 请求方法 目标URL HTTP版本
//  GET /index.html HTTP/1.1
http_conn::HTTP_CODE http_conn::parse_req_line(char *text)
{
    // cout << "url0 = " << text << endl;
    // GET /index.html HTTP/1.1 找到第1个空格或制表符位置
    m_url = strpbrk(text, " \t"); // 在一个字符串中查找某个字符集合第一次出现的位置
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0) // 忽略大小写比较
    { 
        m_method = GET;
    }
    else
    {
        return BAD_REQUEST;
    }

    // /index.html HTTP/1.1 找第二个制表符， 获得m_url和 m_version
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {   // 没有http版本号
        return BAD_REQUEST; // 请求语法错误
    }
    *m_version++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST; // 忽略大小写判断一样
    }

    // http://192.168.110.129:10000/index.html
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;                 // 192.168.110.129:10000/index.html
        m_url = strchr(m_url, '/'); // 在一个字符串中查找某个字符第一次出现的位置
        // murl = /index.html 文件名
    }
    if (!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 主状态机检查状态变成检查请求头
    return NO_REQUEST;                  // 请求不完整，需要继续解析请求报文
}

// 2.解析请求头
// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 遇到空行，表示头部字段解析完毕
    if (text[0] == '\0')
    {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11; // 到空格了
        text += strspn(text, " \t"); // text 指针移动到\t后的第一个的字符处
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");  // 返回text开头到包含\t的长度
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        // 处理Host头部字段 Host: 192.168.198.133:10000
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
}
// 3.解析请求体 没有真正解析请求体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_check_idx))
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 4.具体的处理
// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // /home/now/myweb/resources
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if (stat(m_real_file, &m_file_stat) < 0)
    {
        return NO_RESOURCE; // 服务器没有资源
    }
    // 判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH))
    {
        return FORBIDDEN_REQUEST; // 没有访问权限
    }
    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST; // 请求语法错误
    }

    // 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    // 创建内存映射，资源映射到地址上,写的时候要把地址发送给客户端
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST; // 文件请求,获取文件成功
}

// 对内存映射区执行munmap操作 取消映射一个HTTP连接中的文件
void http_conn::unmap()
{
    if (m_file_address)
    {                                                // 如果文件地址有效
        munmap(m_file_address, m_file_stat.st_size); // 使用 munmap 取消映射
        m_file_address = 0;                          // 将文件地址置为 0，表示取消映射完成
    }
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    { // 写缓冲区已满，无法继续写入数据
        return false;
    }
    va_list arg_list; // va_list类型变量用于存储可变参数列表
    va_start(arg_list, format); //  arg_list 设置为指向可变参数列表中的第一个可变参数。
    // vsnprintf(缓冲区写入数据起始位置，剩余空间，格式"%s %d %s\r\n"， 可变参数列表)
    /*将可变参数列表 arg_list 按照指定的格式 format 进行格式化，并将结果写到缓冲区中，
    最多写入WRITE_BUFFER_SIZE-1-m_write_idx个字符（预留\0）。len 表示实际写入的字符数（不包括'\0'）*/
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    // 如果格式化操作完成，但输出的字符数超过了指定的缓冲区大小-1，那么函数返回的值是指定缓冲区的大小-1
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx) || len < 0)
    {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list); // 清理va_list类型变量arg_list以结束对可变参数列表的访问
    return true;
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{   
    
    switch (ret)
    {
    case INTERNAL_ERROR:
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
        {
            return false;
        }
        break;
    case BAD_REQUEST:
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
        {
            return false;
        }
        break;
    case NO_RESOURCE:
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
        {
            return false;
        }
        break;
    case FORBIDDEN_REQUEST:
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
        {
            return false;
        }
        break;
    case FILE_REQUEST:
        add_status_line(200, ok_200_title);
        add_headers(m_file_stat.st_size);
        m_iv[0].iov_base = m_write_buf; // 起始位置是写缓冲区
        m_iv[0].iov_len = m_write_idx;
        m_iv[1].iov_base = m_file_address; // 目标文件映射到内存中的地址
        m_iv[1].iov_len = m_file_stat.st_size;
        m_iv_count = 2;
        return true;
    default:
        return false;
    }
    // 其他状态值写入缓冲区，不写目标文件地址
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
