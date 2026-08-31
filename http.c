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

int main(int argc, char const *argv[])
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    // 服务器自身
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;                // IPv4
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有网卡
    addr.sin_port = htons(9000);              // 9000端口

    bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    listen(fd, 9);
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

        // 接收 GET POST 等信息都放入buf
        ssize_t size = recv(c_fd, buf, 4096, 0);
        // 端口号：从n网络字节序（大端）转h主机字节序（小端） ， short int
        int port = ntohs(addr_src.sin_port);
        // IP
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr_src.sin_addr, ip, INET_ADDRSTRLEN);

        printf("recv:%s,len=%ld, from:%s:%d\n", buf, size, ip, port);

        // 从buf中处理请求头

        /* 方案一 截取路径思路 GET /huli.png HTTP/1.1，有两个空格 纯指针，不够优雅
        char buf_path[1024];
        char *first = strchr(buf, '/'); // 找第一个/和第二个空格之间的 huli.png
        char *second = strchr(first, ' ');
        int len_path = second - first - 1; // 路径长度

        strncpy(buf_path, first + 1, len_path);
        buf_path[len_path] = '\0';

        int file_fd = open(buf_path, O_RDONLY);
        if (file_fd < 0)
        {
            perror("open file");
            close(c_fd);
            continue;
        }
        要传得文件大小
        struct stat st;
        fstat(file_fd, &st);
        long total_size = st.st_size; */

        // 方案二 使用sscanf
        char method[16], path[256], version[16];

        // 格式：%s 匹配连续非空字符（即GET），%s 匹配路径，%s 匹配HTTP/1.1
        if (sscanf(buf, "%15s %255s %15s", method, path, version) == 3)
        {
            printf("方法: %s\n", method);  // GET
            printf("路径: %s\n", path);    // /hua.png?v=1
            printf("版本: %s\n", version); // HTTP/1.1

            // 接下来处理路径中的 ? 参数
            char *qmark = strchr(path, '?');
            if (qmark != NULL)
                *qmark = '\0'; // 砍掉参数，留下纯净的 /hua.png 即/hua.png?v=1.0&size=100 -> /hua.png\0v=1.0&size=100 ->/hua.png

            // 去掉路径开头的斜杠，得到文件名
            char *file_name = path;
            if (file_name[0] == '/')
                file_name++; // 跳过第一个 /
            // 现在 file_name 就是 "hua.png"

            int file_fd = open(file_name, O_RDONLY);
            if (file_fd < 0)
            {
                perror("open error\n");
                close(c_fd); // 关闭该客户端的连接
                continue;
            }

            // 成功打开，要传得文件大小
            struct stat st;
            fstat(file_fd, &st);
            long total_size = st.st_size;

            // 发送，先发响应头
            char header[256];
            snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: image/webp\r\n"
                     "Content-Length: %ld\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     total_size);
            send(c_fd, header, strlen(header), 0);

            // sendfile 零拷贝发送文件
            // 偏移量参数传入/传出
            off_t offset = 0;
            ssize_t send_len;
            long sent = 0;
            while (sent < total_size)
            {
                // 一次最多发送BUF_SIZE
                send_len = sendfile(c_fd, file_fd, &offset, 4096);
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
        close(c_fd);
    }
    close(fd);
    return 0;
}
