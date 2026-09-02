// http_server
#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

//
#include <sys/un.h>
#include <netinet/in.h>

//
#include <arpa/inet.h>

//
#include <sys/stat.h>
#include <fcntl.h>

//
#include <sys/sendfile.h>

//
#include <pthread.h>

//
#include <time.h>

// 防崩
#include <signal.h>

#define THREAD_NUM 4 // 线程池最大数量
void *worker_routine(void *arg);
void mysendfile(int socket_fd, int file_fd, const char *content_type);
const char *get_content_type(const char *filename);

// 任务节点
typedef struct Task
{
    int client_fd; // 客户端套接字描述符
    struct Task *next;
} Task;

// 线程池
typedef struct ThreadPool
{
    Task *head;           // 任务队头
    Task *tail;           // 任务队尾
    pthread_mutex_t lock; // 互斥锁
    pthread_cond_t cond;  // 条件变量

    int shutdown; // 销毁标志
} ThreadPool;

ThreadPool pool;

/*
    thread_pool_init 函数
    作用：初始化线程池
    参数： 1、线程池结构体变量，2、线程池中线程的数量 THREAD_NUM
*/
void thread_pool_init(ThreadPool *pool, int num_threads)
{
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pool->head = pool->tail = NULL;
    pool->shutdown = 0;

    for (int i = 0; i < num_threads; i++)
    {
        pthread_t tid;
        pthread_create(&tid, NULL, worker_routine, (void *)pool);
    }
}
/*
    worker_routine 函数
    作用：线程工作函数
    参数：1、线程池结构体变量
*/

void *worker_routine(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;
    while (1)
    {
        pthread_mutex_lock(&pool->lock);
        while (pool->head == NULL && !pool->shutdown)
        {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }
        if (pool->shutdown)
        {
            pthread_mutex_unlock(&pool->lock);
            pthread_exit(NULL);
        }

        // 取出队首任务
        Task *task = pool->head;
        pool->head = task->next;
        if (pool->head == NULL)
            pool->tail = NULL;
        pthread_mutex_unlock(&pool->lock);

        // ---- 处理该客户端请求 ----
        int c_fd = task->client_fd;
        char buf[4096] = {0};
        ssize_t size = recv(c_fd, buf, sizeof(buf) - 1, 0);
        if (size <= 0)
        {
            close(c_fd);
            free(task);
            continue;
        }
        // 从buf中处理请求头
        // 使用sscanf
        char method[16], path[256], version[16];
        // 格式：%s 匹配连续非空字符（即GET），%s 匹配路径，%s 匹配HTTP/1.1
        if (sscanf(buf, "%15s %255s %15s", method, path, version) != 3)
        {
            const char *not_found = "HTTP/1.1 400 Not Found\r\nContent-Length: 0\r\n\r\n";
            send(c_fd, not_found, strlen(not_found), 0);
            close(c_fd);
            free(task);
            continue;
        }

        printf("方法: %s\n", method);  // GET POST
        printf("路径: %s\n", path);    // /hua.png?v=1
        printf("版本: %s\n", version); // HTTP/1.1

        // GET
        if (strcmp(method, "GET") == 0)
        {
            // 接下来处理路径中的 ? 参数
            char *qmark = strchr(path, '?');
            if (qmark != NULL)
                *qmark = '\0'; // 砍掉参数，留下纯净的 /hua.png 即/hua.png?v=1.0&size=100 -> /hua.png\0v=1.0&size=100 ->/hua.png

            // 去掉路径开头的斜杠，得到文件名
            char *file_name = path;
            if (strcmp(path, "/") == 0) // 无path纯ip+端口 http://192.168.121.130:9000
            {
                int file_default_fd = open("index.html", O_RDONLY);
                if (file_default_fd < 0)
                {
                    // 发送 404 给客户端，让浏览器知道文件不存在
                    const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                    send(c_fd, not_found, strlen(not_found), 0);
                    close(c_fd);
                    free(task);
                    continue;
                }
                mysendfile(c_fd, file_default_fd, "text/html");
                close(file_default_fd);
                close(c_fd);
                free(task);
                continue;
            }

            else if (file_name[0] == '/') // 有path http://192.168.121.130:9000/huli.png
            {
                file_name++; // 跳过第一个

                // 防止http://192.168.121.130:9000/../../etc/passwd这种恶意访问
                if (strstr(file_name, "..") != NULL)
                {
                    const char *forbidden = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
                    send(c_fd, forbidden, strlen(forbidden), 0);
                    close(c_fd);
                    free(task);
                    continue;
                }

                // 现在 file_name 就是 "hua.png"
                int file_fd = open(file_name, O_RDONLY);
                if (file_fd < 0)
                {
                    perror("open error\n");
                    close(c_fd); // 关闭该客户端的连接
                    free(task);
                    continue;
                }
                const char *content_type = get_content_type(file_name);
                mysendfile(c_fd, file_fd, content_type);
                close(file_fd);
            }
        }

        // POST
        else if (strcmp(method, "POST") == 0)
        {
            // 1. 查找 Content-Length 头部
            char *cl = strstr(buf, "Content-Length: ");
            if (cl == NULL)
            {
                const char *resp = "HTTP/1.1 411 Length Required\r\nContent-Length: 0\r\n\r\n";
                send(c_fd, resp, strlen(resp), 0);
                close(c_fd);
                free(task);
                continue;
            }
            long content_len = atol(cl + 16); // 跳过 "Content-Length: "

            // 2. 查找空行（请求体起始）
            char *body_start = strstr(buf, "\r\n\r\n");
            if (body_start == NULL)
            {
                const char *resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
                send(c_fd, resp, strlen(resp), 0);
                close(c_fd);
                free(task);
                continue;
            }
            body_start += 4; // 跳过 \r\n\r\n

            // 3. 计算已经读到的 body 字节数
            long already = (buf + size) - body_start; // size 是 recv 返回的总字节数
            if (already > content_len)
                already = content_len; // 防止超出

            // 4. 从路径中提取文件名+用时间戳（安全过滤后使用）
            char *file_name = path; // path = "/upload/myphoto.jpg"
            // 去掉开头的 '/'
            if (file_name[0] == '/')
                file_name++;

            // 防止路径穿越（过滤 ..）
            if (strstr(file_name, "..") != NULL)
            {
                const char *resp = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
                send(c_fd, resp, strlen(resp), 0);
                close(c_fd);
                free(task);
                continue;
            }

            // 如果用户只写了 /upload/ 而没有文件名，可以回退到时间戳
            if (strlen(file_name) == 0 || strcmp(file_name, "upload") == 0)
            {
                snprintf(file_name, sizeof(path) - 1, "upload_%ld.dat", time(NULL));
            }
            else
            {
                // 可以保留原始文件名，但建议只允许字母数字点，防止特殊字符
                // 简单做法：直接使用，但过滤掉目录分隔符
                char *p = file_name;
                while (*p)
                {
                    if (*p == '/' || *p == '\\')
                        *p = '_'; // 替换为下划线
                    p++;
                }
            }

            // 然后打开文件（指定路径，放在 uploads/ 目录下）
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "uploads/%s", file_name);
            int fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0)
            {
                perror("open upload file");
                const char *resp = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
                send(c_fd, resp, strlen(resp), 0);
                close(c_fd);
                free(task);
                continue;
            }

            // 5. 写入已经收到的 body 部分
            long total_written = 0;
            if (already > 0)
            {
                ssize_t w = write(fd, body_start, already);
                if (w > 0)
                    total_written += w;
            }

            // 6. 循环接收剩余数据
            while (total_written < content_len)
            {
                ssize_t n = recv(c_fd, buf, sizeof(buf), 0);
                if (n <= 0)
                    break; // 连接断开或出错
                ssize_t w = write(fd, buf, n);
                if (w > 0)
                    total_written += w;
            }

            close(fd);

            // 7. 返回成功响应
            if (total_written == content_len)
            {
                const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
                send(c_fd, resp, strlen(resp), 0);
                printf("文件上传成功: %s (%ld bytes)\n", file_name, total_written);
            }
            else
            {
                const char *resp = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
                send(c_fd, resp, strlen(resp), 0);
                printf("文件上传不完整: %ld/%ld\n", total_written, content_len);
            }

            close(c_fd);
            free(task);
            continue; // 处理完 POST 后回到线程循环
        }

        close(c_fd);
        free(task);
    }
}

/*
    get_content_type 函数
    作用：处理文件后缀名和http协议请求头中Content-Type的映射关系
    参数： 1、文件名，例如/hua.png就知道是png对应image
*/
const char *get_content_type(const char *filename)
{
    // 从文件名中找到最后一个 '.'
    const char *dot = strrchr(filename, '.');
    if (!dot)
        return "application/octet-stream"; // 没有后缀，按二进制流处理
    // 比较后缀（不区分大小写可以用 strcasecmp）
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".webp") == 0)
        return "image/webp";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".js") == 0)
        return "application/javascript";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".svg") == 0)
        return "image/svg+xml";
    if (strcmp(dot, ".pdf") == 0)
        return "application/pdf";
    if (strcmp(dot, ".txt") == 0)
        return "text/plain";
    // ... 可以继续扩展
    return "application/octet-stream"; // 默认
}

/*
    mysendfile 函数
    作用：发送文件
    参数：1、客户端套接字描述符，2、要发送的文件描述符，3、发送的文件类型 例如HTML对应text/html 图片对应image/webp等
*/
void mysendfile(int socket_fd, int file_fd, const char *content_type)
{
    // 成功打开，要传得文件大小
    struct stat st;
    fstat(file_fd, &st);
    long total_size = st.st_size;

    // 发送，先发响应头
    char header[256];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Connection: close\r\n"
             "\r\n",
             content_type, total_size);
    send(socket_fd, header, strlen(header), 0);

    // sendfile 零拷贝发送文件
    // 偏移量参数传入/传出
    off_t offset = 0;
    ssize_t send_len;
    long sent = 0;
    while (sent < total_size)
    {
        // 一次最多发送BUF_SIZE
        send_len = sendfile(socket_fd, file_fd, &offset, 4096);
        if (send_len <= 0)
        {
            perror("sendfile");
            break;
        }
        sent += send_len;
        printf("\r已发送: %ld/%ld", sent, total_size);
    }
    printf("\n文件发送完毕\n");
}

int main(int argc, char const *argv[])
{
     signal(SIGPIPE, SIG_IGN);   // 让系统忽略 SIGPIPE，写错误会返回 -1，而不是杀进程

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    // 服务器自身
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;                // IPv4
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有网卡
    addr.sin_port = htons(9000);              // 9000端口

    bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    listen(fd, 9);

    thread_pool_init(&pool, THREAD_NUM); // 初始化线程池

    printf("服务器准备就绪\n");

    char buf[4096]; // 足够存请求头 GET POST等

    while (1)
    {
        memset(buf, 0, 4096);
        printf("等待连接...\n");

        /*
            你准备好空表（addr_src），喊内核（accept）把客人的身份证号（IP+端口）填上去，
            顺便给了你一个专属通话分机（c_fd）；
            你再用这个分机听客人说话（recv）；
            最后你自己把身份证号翻译成汉字（ntohs + inet_ntop）打印出来。
        */
        // 客户端地址
        struct sockaddr_in addr_src;
        memset(&addr_src, 0, sizeof(addr_src));
        int len = sizeof(addr_src);

        // 将客户端的信息ip，端口等信息写入addr_src结构体
        int c_fd = accept(fd, (struct sockaddr *)&addr_src, &len);
        if (c_fd < 0)
        {
            continue;
        }

        // // 接收 GET POST 等信息都放入buf
        // ssize_t size = recv(c_fd, buf, 4096, 0);
        // // 端口号：从n网络字节序（大端）转h主机字节序（小端） ， short int
        // int port = ntohs(addr_src.sin_port);
        // // IP
        // char ip[INET_ADDRSTRLEN];
        // inet_ntop(AF_INET, &addr_src.sin_addr, ip, INET_ADDRSTRLEN);

        // printf("recv:%s,len=%ld, from:%s:%d\n", buf, size, ip, port);

        // 创建任务
        Task *task = (Task *)malloc(sizeof(Task));
        task->client_fd = c_fd;
        task->next = NULL;

        pthread_mutex_lock(&pool.lock);
        if (pool.tail)
        {
            pool.tail->next = task;
            pool.tail = task;
        }
        else
        {
            pool.head = pool.tail = task;
        }
        pthread_cond_signal(&pool.cond);
        pthread_mutex_unlock(&pool.lock);
    }
    close(fd);
    return 0;
}
