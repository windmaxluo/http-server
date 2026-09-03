# GeekLab HTTP Server

基于 C 语言和原生 Socket 编写的轻量级 HTTP/1.1 静态文件服务器，支持多线程并发、GET/POST 请求、文件上传和零拷贝传输。项目旨在作为嵌入式物联网（IoT）设备的服务端中枢，兼容 ESP32-CAM 图像上传和 STM32 传感器数据上报。

---

## ✨ 功能特性

- **高性能并发**：使用 **线程池（生产者-消费者模型）** 处理客户端请求，支持多连接同时访问。
- **HTTP 协议支持**：
  - **GET**：处理静态资源（HTML、图片、音频、视频等），自动识别 MIME 类型。
  - **POST**：接收文件上传（支持二进制流，如 JPEG、MP3 等），按 `Content-Length` 循环接收并保存到 `uploads/` 目录。
- **零拷贝优化**：利用 `sendfile` 系统调用发送文件，减少内核与用户态数据拷贝，显著提升大文件传输效率。
- **安全防护**：
  - 过滤路径穿越攻击（`..`）。
  - 忽略 `SIGPIPE` 信号，防止客户端意外断开导致进程崩溃。
- **日志与调试**：控制台实时打印请求方法、路径、状态及发送进度。
- **部署友好**：支持 Systemd 服务托管，实现开机自启与异常自愈。

---

## 🧱 技术栈

- **语言**：C（C99）
- **网络**：Socket API、TCP/IP
- **并发**：POSIX 线程（pthread）
- **文件传输**：sendfile（零拷贝）
- **协议解析**：sscanf + 字符串处理（无第三方库依赖）

---

## 📁 目录结构
```plaintext
GeekLab-HTTP-Server/
├── http_server                # 编译后的可执行文件
├── http_server.c              # 源代码（或 http_server.txt）
├── html/                      # 静态网页目录
│   ├── index.html             # 主页面
│   ├── about.html             # 个人介绍
│   ├── works.html             # 作品展示
│   └── data.html              # 硬件数据看板（模拟/真实）
├── image/                     # 图片资源（封面、图标等）
│   └── huli.png
├── music/                     # 音频文件
│   ├── tianya.mp3
│   ├── chuimeng.mp3
│   └── shangxin.mp3
├── uploads/                   # POST 上传文件存储目录（自动创建）
└── README.md                  # 项目说明
```

## 📡 使用说明

### 1. 浏览器访问静态页面

- 主页：`http://<IP>:9000/` 或 `http://<IP>:9000/html/index.html`
- 个人介绍：`http://<IP>:9000/html/about.html`
- 作品展示：`http://<IP>:9000/html/works.html`
- 硬件数据看板：`http://<IP>:9000/html/data.html`

### 2. GET 请求（静态资源）

服务器会根据文件扩展名自动设置 `Content-Type`，支持常见类型：

| 扩展名 | Content-Type |
|--------|--------------|
| `.html` | `text/html` |
| `.png`  | `image/png` |
| `.jpg`  | `image/jpeg` |
| `.mp3`  | `audio/mpeg` |
| `.css`  | `text/css` |
| `.js`   | `application/javascript` |
| 其他    | `application/octet-stream` |

### 3. POST 请求（文件上传）

使用 `curl` 测试：

```bash
curl -X POST --data-binary @/path/to/local/file.jpg http://<IP>:9000/upload/myphoto.jpg
```

## 📝 待优化与扩展
- 支持 Transfer-Encoding: chunked 分块传输
- 支持 multipart/form-data 解析
- 实现访问日志（access.log）
- 添加配置文件（端口、根目录、线程数等）

## 📄 许可证
    本项目仅供学习交流使用，可自由修改和分发