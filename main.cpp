#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include <signal.h>
#include "http_conn.h"
#include <cassert>


#define MAX_FD 65535    // 最大的fd数量
#define MAX_EVENT_NUMBER 10000 // 一次监听的最大事件数目

int pipefd[2];
sort_timer_list timer_lst;
int epollfd;

void addsig(int sig, void ( handler )(int))
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

// 重载函数，提供定时检测非活使用
void addsig( int sig )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void timer_handler()
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}



// 添加设置非阻塞函数
extern int setnonblocking(int fd);

// 添加文件描述符到epoll中
extern void addfd(int epollfd, int fd, bool oneshot);

// 从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);

// 修改文件描述符
extern void modfd(int epollfd, int fd, int ev);

int main(int argc, char * argv[])
{

    // 使用命令行指定端口等信息
    if (argc <= 1) {

        printf("按照如下格式运行: %s port_number\n", basename(argv[0]));

        exit(-1);

    }

    // 获取端口号
    int port = atoi(argv[1]);

    // 对sigpie信号进行处理
    addsig(SIGPIPE, SIG_IGN); // 对信号进行忽略

    // 创建线程池，初始化信息
    threadpool<http_conn> * pool = NULL;

    try {

        pool = new threadpool<http_conn>;

    } catch(...) {

        exit(-1);

    }

    // 使用数组保存所有的客户端信息
    http_conn * requestArr = new http_conn[MAX_FD];

    // 网络部分的代码
    int listenfd =socket(AF_INET, SOCK_STREAM, 0);

    
    

    // 绑定端口
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    // 设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int ret = bind(listenfd, (struct sockaddr *) &addr, sizeof(addr));

    ret = listen(listenfd, 128);



    // 创建epoll,事件数组，存储epoll的存储事件的对象，相比于前面两个来说，已经好了很多了
    epoll_event events[MAX_EVENT_NUMBER];

    // 开辟epollevent区域，包含了红黑树的所有事件，以及链表的有修改的部分
    epollfd = epoll_create(200);
    // 将监听的文件描述符添加到epoll对象中 // 注册读就绪事件
    addfd(epollfd, listenfd, false);

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0 ,pipefd);
    assert( ret != -1 );
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], true);

    // 添加两种信号
    addsig( SIGALRM );
    addsig( SIGTERM );
    bool stop_server = false;

    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号

    http_conn::m_epollfd = epollfd;
    while (! stop_server)
    {
        // 主线程不断循环检测事件的发生
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (num < 0 && errno != EINTR) {
            printf("epoll failure");
            break;
        }
        //循环遍历事件数组
        for (int i = 0; i < num; ++ i) {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd) {
                
                // 有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t clientaddrlen = sizeof(client_address);
                // 传入式参数
                int connectfd = accept(listenfd, (struct sockaddr*)&client_address, &clientaddrlen);
                if ( connectfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                }

                if (http_conn::m_user_count >= MAX_FD) {
                    // 目前连接数量满了，应该给客户端提示
                    // 给客户端一个信息，服务器正忙
                    close(connectfd);
                    continue;
                }

                // 将这个描述符加入到数组中，将新的客户的数据初始化，放到数组中
                requestArr[connectfd].init(connectfd, client_address, timer_lst);
                
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 对方异常断开或者错误事件, 直接关闭连接，处理对应的事件

                requestArr[sockfd].close_conn();

            } 
            // pipefd[0]触发的读入事件
            else if ( (sockfd == pipefd[0]) && (events[i].events & EPOLLIN) )
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) continue;
                else if (ret == 0) continue;
                else {
                    for (int i = 0; i < ret; ++ i) {
                        switch ( signals[i] ){
                            case SIGALRM:
                            // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                            // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            case SIGTERM:
                                stop_server = true;
                        }
                    }
                }
            }
            else if (events[i].events & EPOLLIN) { 
                if (requestArr[sockfd].read(epollfd ,timer_lst)) {
                    // 一次性将所有的数据都读出来
                    pool->append(requestArr + sockfd);
                } else {
                    requestArr[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT) // 写
            {
                if (!requestArr[sockfd].write()) {
                    requestArr[sockfd].close_conn();
                }
            }
        }
        // 最后处理定时时间，因为I/0有更高的优先级
        if (timeout) {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] requestArr;
    delete pool;
    return 0;
}