#include "http_conn.h"


const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

const char * root = "/home/controller/linux/webserver/resources";

extern int epollfd;

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func( http_conn* user_data )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->getfd(), 0 );
    assert( user_data );
    close( user_data->getfd() );
    printf( "close fd %d\n", user_data->getfd() );
}


// 设置文件描述符非阻塞
int setnonblocking(int fd) {
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}


// 向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool oneshot) {
    epoll_event event;
    event.data.fd = fd;
    // 对方连接断开，就会触发EPOLLRDHUP，之前是通过read函数的返回值是否为0来判断是否断开连接
    //event.events = EPOLLIN | EPOLLRDHUP; // 好的服务器可以支持边缘模式，也可以使用水平模式
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET; // 设置为边沿触发
    if(oneshot) {
        event.events |= EPOLLONESHOT; // 如果开启了oneshot模式，那么socket连接在任何时刻都只能够被一个线程处理
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd); // 因为ET只能够在非阻塞的情况下使用，否则
}

void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT,确保下一次可读时EPOLLIN可以触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT  | EPOLLRDHUP; //重新注册这个事件
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, (epoll_event *)&event);
}


int http_conn::m_epollfd = -1; // 所有socket上的事件都被注册到同一个epoll对象中
int http_conn::m_user_count = 0; // 统计用户的数量

void http_conn::close_conn() {
    // 关闭连接
    if (m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count --; // 关闭一个连接，客户总数 - 1
    }
}

void http_conn::init(int sockfd, const sockaddr_in & addr, sort_timer_list& timer_lst)
{
    m_address = addr;
    m_sockfd = sockfd;
    // 设置端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到epoll对象中
    addfd(m_epollfd, sockfd, true); // oneshot事件的添加
    m_user_count ++;

    init();

    // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
    util_timer* timer = new util_timer;
    timer->user_data = this;
    timer->cb_func = cb_func;
    time_t cur = time( NULL );
    timer->expire = cur + 3 * TIMESLOT;
    this->timer = timer;
    timer_lst.add_timer( timer );
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始状态为解析请求首行
    m_checked_index = 0;
    m_start_line = 0;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_linger = false;
    m_host = 0;
    m_read_idx = 0;
    m_write_idx = 0;


    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

bool http_conn::read(int epollfd, sort_timer_list& timer_lst)
{
    // 循环读取客户的数据，直到无数据可读或者关闭连接
    if(m_read_idx >= READ_BUFFER_SIZE) return false;
    // 读取到的字节
    int bytes_read = 0;
    util_timer *timer = this->timer;
    while(true) {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, 
        READ_BUFFER_SIZE - m_read_idx, 0 );
        if (bytes_read == -1) {
            if( errno == EAGAIN || errno == EWOULDBLOCK ) {
                // 没有数据
                break;
            }
            cb_func(this);
            if (timer)
            {
                timer_lst.del_timer( timer );
            }
            return false;   
        } else if (bytes_read == 0) {   // 对方关闭连接
            cb_func(this);
            if (timer)
            {
                timer_lst.del_timer( timer );
            }
            return false;
        } else {
            if ( timer )
            {
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                printf("调整时间一次\n");
                timer_lst.adjust_timer( timer );
            }
        }
        m_read_idx += bytes_read;
    }
    return true;
}

http_conn::LINE_STATE http_conn::parse_line() {
    char temp;
    for ( ; m_checked_index < m_read_idx; ++m_checked_index ) {
        temp = m_read_buf[ m_checked_index ];
        if ( temp == '\r' ) {
            if ( ( m_checked_index + 1 ) == m_read_idx ) {
                return LINE_OPEN;
            } else if ( m_read_buf[ m_checked_index + 1 ] == '\n' ) {
                m_read_buf[ m_checked_index++ ] = '\0';
                m_read_buf[ m_checked_index++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if( temp == '\n' )  {
            if( ( m_checked_index > 1) && ( m_read_buf[ m_checked_index - 1 ] == '\r' ) ) {
                m_read_buf[ m_checked_index-1 ] = '\0';
                m_read_buf[ m_checked_index++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

http_conn::HTTP_CODE http_conn::parse_request_line(char * text) {
    // GET / HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断空格和\t哪个先出现，m_url是该字符的地址
    if (! m_url) { 
        return BAD_REQUEST;
    }
    *m_url ++ = '\0';

    char * method = text;
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else return BAD_REQUEST;
    
    m_version = strpbrk(m_url, " \t");
    if (!m_version) return BAD_REQUEST;
    *m_version ++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }
    // http://192.168.1.1:10000/index.html
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7; // 192.168.1.1:10000/index.html
        m_url = strchr(m_url, '/'); // 找到 /index.html
    } 

    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER; //主状态机检查状态变成检查请求头
    return NO_REQUEST;

}

http_conn::HTTP_CODE http_conn::parse_headers(char * text) {
    if(text[0] == '\0') {
        // 如果当前的HTTP请求有消息体，那么还需停药读取m_content_length字节的消息体
        // 状态机转移到CHECK_STATE_CONTENT的状态
        if(m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则直接就解析完成，说明解析到的是空行
    } else if (strncasecmp( text, "Connection:", 11 ) == 0) {
        // 处理头部字段 Connection: keep-alive
        text += 11; // 指针向后移动11位
        text += strspn( text, " \t");// 找到对应的指针部分
        if (strcasecmp( text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if (strncasecmp( text, "Content-Length:", 15 ) == 0) {
        text += 15;
        text += strspn( text, " \t");
        m_content_length = atol(text); // 这里更新m_content_length;
    } else if (strncasecmp( text, "Host", 5) == 0) {
        text += 5; 
        text += strspn( text, " \t");
        m_host = text;
    } else {
        // 解析所有的头部信息
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

// 我们只是简单的判断其是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char * text) {
    if (m_read_idx >= (m_content_length + m_checked_index))
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析整个请求，会用到下面的函数和方法
http_conn::HTTP_CODE http_conn::process_read() {

    LINE_STATE line_state = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char * text = nullptr;

    while (((m_check_state == CHECK_STATE_CONTENT) && (line_state == LINE_OK)) || 
             ((line_state = parse_line()) == LINE_OK)) // 正常读取
    {
        // 当前表示解析到了一行完整的数据，或者解析到了请求体，也是完成的数据
        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_index; // ？ 为什么这儿是这个，这个m_check_index是在哪儿改变的
        printf("get one line : %s\n", text);
        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text); // 获取到一行数据，交给其进行解析，这个数据是请求头中的某一行
                
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {
                    // 获取一个完整的请求头
                    return do_request();
                }
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST) { // 成功
                    return do_request(); // 将资源，URL等其出来，做下一步的操作。
                }
                line_state = LINE_OPEN; // 失败了
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    // 如果得到一个完整的，正确的HTTP请求时，我们就分析目标文件的属性，如果目标文件存在，对所有
    // 用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
    strcpy(m_real_file, root);
    int len = strlen(root);
    // m_real_file : http://192.168.44.138 在m_real_file的后面贴上url
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    // 获取当前文件的相关的状态信息码，-1表示失败，0表示成功
    if (stat( m_real_file, &m_file_stat) < 0){
        return NO_REQUEST;
    }

    // 判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }
    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY);
    // 内存映射
    m_file_address = ( char*) mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close( fd );
    return FILE_REQUEST;
}

void http_conn::unmap() {
    if (m_file_address)
    {
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;    // 已经发送的字节
    int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数
    if (bytes_to_send == 0) {
        // 将要发送的字节位0，这一次相应结束.
        modfd( m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    while (1) 
    {
        // 分散内存块的写，将数据写入到m_iv中去
        temp = writev(m_sockfd, m_iv, m_iv_count); // 将数据写出去
        if (temp <= -1) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if (errno == EAGAIN) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false; // 这是无法搞定的情况，因此直接返回false
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send ) {
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

bool http_conn::add_response( const char* format, ...  ) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) return false;
    va_list arg_list;
    va_start( arg_list, format);
    // 往writebuf中写入数据
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        return false;
    }
    m_write_idx += len; // 加上长度
    va_end( arg_list);
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 获取写的情况
bool http_conn::process_write( HTTP_CODE read_code )
{
    switch(read_code)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen(error_500_form));
            if (!add_content(error_500_form)) return false;
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title);
            add_headers( strlen(error_400_form));
            if (!add_content(error_400_form)) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (! add_content(error_404_form)) return false;
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf; // 这个操作将头和内容分成了两段 第一段是头
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address; // 第二段是文件的地址，也就是内容
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            return true;
        default:
            return false;
    }
    m_iv[ 0 ].iov_base = m_write_buf; // 如果不是file_request，那么其他的请求也要有头部信息，因此需要buffer
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程处理，处理HTTP请求的入口函数
void http_conn::process()
{
    // 解析HTTP请求要用到有限状态机
    
    HTTP_CODE read_code =  process_read();
    if (read_code == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN); // 读取数据不完整，还需要再去修改，加上oneshot
        return;
    }
    
    // 生成响应
    bool write_ret = process_write( read_code );
    if (!write_ret) {
        close_conn();
    }
    // 注册写事件
    modfd( m_epollfd, m_sockfd, EPOLLOUT); 
}


