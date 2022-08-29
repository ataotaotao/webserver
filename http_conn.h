#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <exception>
#include "locker.h"
#include <cstring>
#include <sys/uio.h>
#include <iostream>
#include <cassert>

using namespace std;
#define TIMESLOT 5

class util_timer;
class sort_timer_list;

class http_conn {

public:
    static const int READ_BUFFER_SIZE = 2048; //读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 2048; // 写缓冲区的大小
    static const int FILENAME_LEN = 200;

    
    http_conn() {};
    ~http_conn(){};
    static int m_epollfd; // 所有socket上的事件都被注册到同一个epoll对象中
    static int m_user_count; // 统计用户的数量
    
    // 状态的设置，以使用状态机
    enum METHOD 
    {
        GET = 0, 
        POST, 
        HEAT, 
        PUT, 
        DELETE, 
        TRACE, 
        OPTIONS,
        CONNECT
    };

    /*
        解析客户端请求时，主状态机的状态 ,也就是解析整体报文的时候的状态
        CHECK_STATE_REQUESTLINE: 当前正在分析请求行
        CHECK_STATE_HEADER: 当前正在分析头部字段
        CHECK_STATE_CONTENT: 当前正在解析请求体
    */
    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0, 
        CHECK_STATE_HEADER, 
        CHECK_STATE_CONTENT
    };
    /*
        从状态机的三种状态，即每一行的读取状态
        LINE_OK: 读取完整的一整行
        LINE_BAD: 读取行出错
        LINE_OPEN: 行数据尚且不完成，还没检查完
    */
    enum LINE_STATE {
        LINE_OK = 0, 
        LINE_BAD, 
        LINE_OPEN
    };

    /*
        服务器处理HTTP请求的可能结果，也就是报文解析的结果
        NO_REQUEST          :       请求不完整，需要继续读取客户端数据
        GET_REQUEST         :       表示获得了一个完整的客户请求
        BAD_REQUEST         :       表示客户请求语法错误
        NO_RESOURCE         :       表示服务器没有资源
        FORBIDDEN_REQUEST   :       表示客户对资源没有足够的访问权限
        FILE_REQUEST        :       文件请求，获取文件成功
        INTERNAL_ERROR      :       文件内部错误
        CLOSED_CONNECTION   :       表示客户端已经关闭连接
    */
   /* 服务器的请求可能比这个多很多，这个只是列出了常用的一些code信息 */
    enum HTTP_CODE {
        NO_REQUEST = 0,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };

    int getfd() {return m_sockfd;}
    void init(int sockfd, const sockaddr_in & addr, sort_timer_list& timer_lst); // 初始化新接收的连接
    void process(); // 处理客户端的请求
    void close_conn();
    bool read(int eppllfd, sort_timer_list& timer_lst);
    bool write(); // 非阻塞的读和写
    char * get_line() {return m_read_buf + m_start_line; }
    HTTP_CODE do_request();
    
private:
    
    HTTP_CODE process_read(); // 解析HTTP请求
    bool process_write( HTTP_CODE read_code );
    HTTP_CODE parse_request_line(char * text); // 解析请求首行
    HTTP_CODE parse_headers(char * text); // 解析HTTP请求头
    HTTP_CODE parse_content(char * text); // 解析HTTP请求体
    LINE_STATE parse_line(); // 解析一行
    void init(); // 初始化连接其余的信息
    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_content_type();
    bool add_status_line( int status, const char* title );
    bool add_headers( int content_length );
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line();
    void unmap();


    int m_sockfd; // 该http连接的socket；
    char m_real_file[FILENAME_LEN];
    
    sockaddr_in m_address; // 通信的socket地址
    char m_read_buf[READ_BUFFER_SIZE]; //读缓冲区的大小
    int m_read_idx; // 读缓冲区中已经读入的客户端数据的最后一个字节的下标
    int m_checked_index; //当前正在分析的字符在缓冲区的位置
    int m_start_line; // 当前正在解析的行的起始位置
    int m_content_length;
    // 解析请求目标文件的文件头
    char * m_url; // url
    char * m_version; // 协议版本HTTP1.1
    METHOD m_method;
    char * m_host;
    bool m_linger; // HTTP请求是否要保持连接
    struct stat m_file_stat;                // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    char* m_file_address;                   // 客户请求的目标文件被mmap到内存中的起始位置

    CHECK_STATE m_check_state;// 主状态机当前所属的状态

    

    char m_write_buf[ WRITE_BUFFER_SIZE ];  // 写缓冲区
    int m_write_idx;                        // 写缓冲区中待发送的字节数

    struct iovec m_iv[2];                   // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;
    util_timer* timer;          // 定时器
};



// 定时器类
class util_timer {
public:
    util_timer() :prev(NULL), next(NULL) {};
    time_t expire; // 任务超时时间
    void (*cb_func)(http_conn*); // 任务回调函数，回调函数处理的客户数量
    http_conn *user_data;     // 用户数据
    util_timer* prev;           // 前一个定时器
    util_timer* next;           // 后一个定时器

};




class sort_timer_list {
public:
    sort_timer_list():head(NULL), tail(NULL){}
    // 链表被销毁
    ~sort_timer_list(){
        util_timer* temp = head;
        while (temp) {
            head = temp->next;
            delete temp;
            temp = head;
        }
    }
    // 添加节点
    void add_timer( util_timer* timers) {
        if (!timers) {
            return;
        }
        if (!head) {
            head = tail = timers;
            return;
        }
        // 如果目标定时器的超时时间小于当前链表中所有定时器的超时时间，则把该定时器插入链表头部，作为链表新的头节点，
        // 否则就需要调用重载函数 add_timer(), 把它插入到链表中合适的位置，以保证连败哦的升序特性
        if (timers->expire < head->expire) {
            // 这个节点要插在头部
            timers->next = head;
            head->prev = timers;
            head = timers;
            return;
        }
        // 如果不在头部，那么就add_timer(timers, head);
        add_timer(timers, head);
    }

    // 删除节点
    void del_timer( util_timer* timer) {
        if (!timer) return;

        // 只有一个定时器，且位目标定时器，那么就直接删除
        if ((head == timer) && (tail == timer)) {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        // 如果至少有两个定时器，且头节点是目标节点
        if (timer == head) {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }

        // 如果至少有两个定时器，且目标节点为tail
        if (timer == tail) {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }

        // 如果目标节点位于链表的中间，则把前后的定时器进行处理即可，删除目标定时器
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    //当某个定时任务发生变化时，调整对应的定时器在链表中的位置。这个函数只考虑被调整的定时器的
    //超时时间延长的情况，即该定时器需要往链表的尾部移动。改变之后的时间只会越来越大
    void adjust_timer(util_timer* timer) {
        // 某个定时器发生变化，需要做调整
        if (!timer) {
            return;
        }
        util_timer* tmp = timer->next;
        if (!tmp || timer->expire < tmp->expire) {
            // 如果被调整的目标定时器处在链表的尾部，定时器新的超时时间仍然小于其下一个定时器的超时时间则不用调整
            return;
        }

        if (timer == head) {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        } else {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer( timer, timer->next ); // 不用从头搜索，只需要继续往后插入即可
        }
    }

    // SIGALARM 信号每次被触发就在其信号处理函数中执行一次 tick() 函数，以处理链表上到期任务。 
    void tick() {
        if ( !head ) {
            return;
        }
        printf( "timer tick\n" );
        time_t cur = time( NULL ); // 获取当前时间
        util_timer* temp = head;
        while (temp) {
            // 因为每个定时器都使用绝对时间作为超时值，所以可以把定时器的超时值和系统当前时间，比较以判断定时器是否到期 
            if (cur < temp->expire) { // 当前所有的节点均没有过期，当前时间小于所有节点的过期时间
                break; // 没有过期
            }
            // 调用定时器回调，执行定时任务
            temp->cb_func( temp->user_data );
            
            // 执行完定时任务，就将他从链表中删除，并重置链表头节点
            head = temp->next;

            if (head) head->prev = NULL;
            delete temp;
            temp = head;
        }
    }

private:
    void add_timer(util_timer *timers, util_timer* lst_head) {
        util_timer* prev = lst_head;
        util_timer* tmp = prev->next;
        while(tmp) {
            if (timers->expire < tmp->expire) {
                prev->next = timers;
                timers->next = tmp;
                tmp->prev = timers;
                timers->prev = prev;
                break; // 添加完毕，退出
            }
            prev = tmp;
            tmp = tmp->next;
        }
        if (!tmp) { // 遍历了lst_head之后没有找到对应的节点，说明链表中存在的节点的过期时间都小于这个timers，因此，直接放到最后即可
            prev->next = timers;
            timers->prev = prev;
            timers->next = NULL;
            tail = timers;
        }
    }
    util_timer* head;
    util_timer* tail;
};




# endif