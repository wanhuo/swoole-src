/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "swoole.h"
#include "Server.h"
#include "Http.h"

#include <sys/stat.h>

static int swUDPThread_start(swServer *serv);

static int swReactorThread_loop_udp(swThreadParam *param);
static int swReactorThread_loop_tcp(swThreadParam *param);
static int swReactorThread_loop_unix_dgram(swThreadParam *param);

static int swReactorThread_onPipeWrite(swReactor *reactor, swEvent *ev);
static int swReactorThread_onClose(swReactor *reactor, swEvent *event);
static int swReactorThread_send_string_buffer(swReactorThread *thread, swConnection *conn, swString *buffer);
static int swReactorThread_send_in_buffer(swReactorThread *thread, swConnection *conn);
static int swReactorThread_get_package_length(swServer *serv, void *data, uint32_t size);

#ifdef SW_USE_RINGBUFFER
static sw_inline void* swReactorThread_alloc(swReactorThread *thread, uint32_t size)
{
    void *ptr = NULL;
    int try_count = 0;

    while (1)
    {
        ptr = thread->buffer_input->alloc(thread->buffer_input, size);
        if (ptr == NULL)
        {
            if (try_count > SW_RINGBUFFER_WARNING)
            {
                swWarn("memory pool is full. Wait memory collect. alloc(%d)", size);
            }
            swYield();
            continue;
        }
        break;
    }
    return ptr;
}
#endif

/**
 * for udp
 */
int swReactorThread_onPackage(swReactor *reactor, swEvent *event)
{
    int ret;
    swServer *serv = reactor->ptr;
    swFactory *factory = &(serv->factory);
    swDispatchData task;

    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    while (1)
    {
        ret = recvfrom(event->fd, task.data.data, SW_BUFFER_SIZE, 0, (struct sockaddr *) &addr, &addrlen);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return SW_ERR;
        }
        break;
    }
    task.data.info.len = ret;

    //UDP的from_id是PORT，FD是IP
    task.data.info.type = SW_EVENT_UDP;
    task.data.info.from_fd = event->fd; //from fd
    task.data.info.from_id = ntohs(addr.sin_port); //转换字节序
    task.data.info.fd = addr.sin_addr.s_addr;
    task.target_worker_id = -1;

    swTrace("recvfrom udp socket.fd=%d|data=%s", event->fd, task.data.data);

    ret = factory->dispatch(factory, &task);
    if (ret < 0)
    {
        swWarn("factory->dispatch[udp packet] failed");
    }
    return SW_OK;
}

/**
 * close the connection
 */
static int swReactorThread_onClose(swReactor *reactor, swEvent *event)
{
    if (SwooleG.serv->factory_mode == SW_MODE_SINGLE)
    {
        return swReactorProcess_onClose(reactor, event);
    }

    int fd = event->fd;
    swDataHead notify_ev;
    bzero(&notify_ev, sizeof(notify_ev));

    notify_ev.from_id = reactor->id;
    notify_ev.fd = fd;
    notify_ev.type = SW_EVENT_CLOSE;

    swConnection *conn = swServer_connection_get(SwooleG.serv, fd);
    if (conn == NULL || conn->active == 0)
    {
        return SW_ERR;
    }

    if (reactor->del(reactor, fd) == 0)
    {
        conn->active |= SW_STATE_REMOVED;
        return SwooleG.factory->notify(SwooleG.factory, &notify_ev);
    }
    else
    {
        return SW_ERR;
    }
}

/**
 * receive data from worker process pipe
 */
int swReactorThread_onPipeReceive(swReactor *reactor, swEvent *ev)
{
    int n;
    swEventData resp;
    swSendData _send;

    swPackage_response pkg_resp;
    swWorker *worker;

    //while(1)
    {
        n = read(ev->fd, &resp, sizeof(resp));
        if (n > 0)
        {
            memcpy(&_send.info, &resp.info, sizeof(resp.info));
            if (_send.info.from_fd == SW_RESPONSE_SMALL)
            {
                _send.data = resp.data;
                _send.length = resp.info.len;
                swReactorThread_send(&_send);
            }
            else
            {
                memcpy(&pkg_resp, resp.data, sizeof(pkg_resp));
                worker = swServer_get_worker(SwooleG.serv, pkg_resp.worker_id);

                _send.data = worker->send_shm;
                _send.length = pkg_resp.length;

                swReactorThread_send(&_send);
                worker->lock.unlock(&worker->lock);
            }
        }
        else if (errno == EAGAIN)
        {
            return SW_OK;
        }
        else
        {
            swWarn("read(worker_pipe) failed. Error: %s[%d]", strerror(errno), errno);
            return SW_ERR;
        }
    }
    return SW_OK;
}

int swReactorThread_send2worker(void *data, int len, uint16_t target_worker_id)
{
    swServer *serv = SwooleG.serv;
    swReactorThread *thread = swServer_get_thread(serv, SwooleTG.id);

    int ret = -1;
    swWorker *worker = &(serv->workers[target_worker_id]);

    if (serv->ipc_mode == SW_IPC_MSGQUEUE)
    {
        swQueue_data *in_data = (swQueue_data *) ((void *) data - sizeof(long));

        //加1,消息队列的type必须不能为0
        in_data->mtype = target_worker_id + 1;
        ret = serv->read_queue.in(&serv->read_queue, in_data, len);
    }
    else
    {
        int pipe_fd = worker->pipe_master;
        swBuffer *buffer = *(swBuffer **) swArray_fetch(thread->buffer_pipe, worker->pipe_master);
        if (swBuffer_empty(buffer))
        {
            ret = write(pipe_fd, (void *) data, len);
            if (ret < 0 && errno == EAGAIN)
            {
                if (serv->connection_list[pipe_fd].from_id == SwooleTG.id)
                {
                    thread->reactor.set(&thread->reactor, pipe_fd, SW_FD_PIPE | SW_EVENT_READ | SW_EVENT_WRITE);
                }
                else
                {
                    thread->reactor.add(&thread->reactor, pipe_fd, SW_FD_PIPE | SW_EVENT_WRITE);
                }
                goto append_pipe_buffer;
            }
        }
        else
        {
            append_pipe_buffer:
            if (buffer->length > SwooleG.unixsock_buffer_size)
            {
                swWarn("Fatal Error: unix socket buffer overflow");
                return SW_ERR;
            }
            if (swBuffer_append(buffer, data, len) < 0)
            {
                swWarn("append to pipe_buffer failed.");
                return SW_ERR;
            }
            return SW_OK;
        }
    }
    return ret;
}

/**
 * send to client or append to out_buffer
 */
int swReactorThread_send(swSendData *_send)
{
    swServer *serv = SwooleG.serv;

    int fd = _send->info.fd;
    uint16_t reactor_id = 0;

    volatile swBuffer_trunk *trunk;

    swConnection *conn = swServer_connection_get(serv, fd);
    if (conn == NULL || conn->active == 0)
    {
        swWarn("Connection[fd=%d] is not exists.", fd);
        return SW_ERR;
    }

#if SW_REACTOR_SCHEDULE == 2
    reactor_id = fd % serv->reactor_num;
#else
    reactor_id = conn->from_id;
#endif

    swTraceLog(SW_TRACE_EVENT, "send-data. fd=%d|reactor_id=%d", fd, reactor_id);
    swReactor *reactor = &(serv->reactor_threads[reactor_id].reactor);

    if (conn->out_buffer == NULL)
    {
        /**
         * close connection.
         */
        if (_send->info.type == SW_EVENT_CLOSE)
        {
            close_fd:
            swServer_connection_close(serv, fd);
            return SW_OK;
        }
#ifdef SW_REACTOR_SYNC_SEND
        //Direct send
        if (_send->info.type != SW_EVENT_SENDFILE)
        {
            int n;

            direct_send:
            n = swConnection_send(conn, _send->data, _send->length, 0);
            if (n == _send->length)
            {
                return SW_OK;
            }
            else if (n > 0)
            {
                _send->data += n;
                _send->length -= n;
                goto buffer_send;
            }
            else if (errno == EINTR)
            {
                goto direct_send;
            }
            else
            {
                goto buffer_send;
            }
        }
#endif
        //Buffer send
        else
        {
#ifdef SW_REACTOR_SYNC_SEND
            buffer_send:
#endif
            conn->out_buffer = swBuffer_new(SW_BUFFER_SIZE);
            if (conn->out_buffer == NULL)
            {
                return SW_ERR;
            }
        }
    }

    //listen EPOLLOUT event
    if (reactor->set(reactor, fd, SW_EVENT_TCP | SW_EVENT_WRITE | SW_EVENT_READ) < 0
            && (errno == EBADF || errno == ENOENT))
    {
        goto close_fd;
    }

    //close connection
    if (_send->info.type == SW_EVENT_CLOSE)
    {
        trunk = swBuffer_new_trunk(conn->out_buffer, SW_TRUNK_CLOSE, 0);
        trunk->store.data.val1 = _send->info.type;
    }
    //sendfile to client
    else if (_send->info.type == SW_EVENT_SENDFILE)
    {
        swConnection_sendfile(conn, _send->data);
    }
    //send data
    else
    {
        /**
         * TODO: Connection output buffer overflow, close the connection.
         */
        if (conn->out_buffer->length >= serv->buffer_output_size)
        {
            swWarn("Connection output buffer overflow.");
        }
        //buffer enQueue
        swBuffer_append(conn->out_buffer, _send->data, _send->length);
    }
    return SW_OK;
}

/**
 * [ReactorThread] worker pipe can write.
 */
static int swReactorThread_onPipeWrite(swReactor *reactor, swEvent *ev)
{
    int ret;
    swReactorThread *thread = swServer_get_thread(SwooleG.serv, SwooleTG.id);
    swBuffer *buffer = *(swBuffer **) swArray_fetch(thread->buffer_pipe, ev->fd);
    swBuffer_trunk *trunk = NULL;

    while (!swBuffer_empty(buffer))
    {
        trunk = swBuffer_get_trunk(buffer);
        ret = write(ev->fd, trunk->store.ptr, trunk->length);
        if (ret < 0)
        {
            return errno == EAGAIN ? SW_OK : SW_ERR;
        }
        else
        {
            swBuffer_pop_trunk(buffer, trunk);
        }
    }

    //remove EPOLLOUT event
    if (swBuffer_empty(buffer))
    {
        if (SwooleG.serv->connection_list[ev->fd].from_id == SwooleTG.id)
        {
            ret = reactor->set(reactor, ev->fd, SW_FD_PIPE | SW_EVENT_READ);
        }
        else
        {
            ret = reactor->del(reactor, ev->fd);
        }
        if (ret < 0)
        {
            swSysError("reactor->set() failed.");
        }
    }
    return SW_OK;
}

int swReactorThread_onWrite(swReactor *reactor, swEvent *ev)
{
    int ret;
    swServer *serv = SwooleG.serv;

    swConnection *conn = swServer_connection_get(serv, ev->fd);
    if (conn->active == 0)
    {
        return SW_OK;
    }

    swBuffer_trunk *trunk;

    while (!swBuffer_empty(conn->out_buffer))
    {
        trunk = swBuffer_get_trunk(conn->out_buffer);
        if (trunk->type == SW_TRUNK_CLOSE)
        {
            close_fd:
            swServer_connection_close(serv, ev->fd);
            return SW_OK;
        }
        else if (trunk->type == SW_TRUNK_SENDFILE)
        {
            swTask_sendfile *task = trunk->store.ptr;

            if (task->offset == 0 && serv->open_tcp_nopush)
            {
                /**
                 * disable tcp_nodelay
                 */
                if (serv->open_tcp_nodelay)
                {
                    int tcp_nodelay = 0;
                    if (setsockopt(ev->fd, IPPROTO_TCP, TCP_NODELAY, (const void *) &tcp_nodelay, sizeof(int)) == -1)
                    {
                        swWarn("setsockopt(TCP_NODELAY) failed. Error: %s[%d]", strerror(errno), errno);
                    }
                }
                /**
                 * enable tcp_nopush
                 */
                if (swSocket_tcp_nopush(ev->fd, 1) == -1)
                {
                    swWarn("swSocket_tcp_nopush() failed. Error: %s[%d]", strerror(errno), errno);
                }
            }

            int sendn = (task->filesize - task->offset > SW_SENDFILE_TRUNK) ? SW_SENDFILE_TRUNK : task->filesize - task->offset;
            ret = swoole_sendfile(ev->fd, task->fd, &task->offset, sendn);
            swTrace("ret=%d|task->offset=%ld|sendn=%d|filesize=%ld", ret, task->offset, sendn, task->filesize);

            if (ret <= 0)
            {
                switch (swConnection_error(errno))
                {
                case SW_ERROR:
                    swWarn("sendfile failed. Error: %s[%d]", strerror(errno), errno);
                    swBuffer_pop_trunk(conn->out_buffer, trunk);
                    return SW_OK;
                case SW_CLOSE:
                    goto close_fd;
                default:
                    break;
                }
            }
            //sendfile finish
            if (task->offset >= task->filesize)
            {
                swBuffer_pop_trunk(conn->out_buffer, trunk);
                close(task->fd);
                sw_free(task);

                if (serv->open_tcp_nopush)
                {
                    /**
                     * disable tcp_nopush
                     */
                    if (swSocket_tcp_nopush(ev->fd, 0) == -1)
                    {
                        swWarn("swSocket_tcp_nopush() failed. Error: %s[%d]", strerror(errno), errno);
                    }
                    /**
                     * enable tcp_nodelay
                     */
                    if (serv->open_tcp_nodelay)
                    {
                        int value = 1;
                        if (setsockopt(ev->fd, IPPROTO_TCP, TCP_NODELAY, (const void *) &value, sizeof(int)) == -1)
                        {
                            swWarn("setsockopt(TCP_NODELAY) failed. Error: %s[%d]", strerror(errno), errno);
                        }
                    }
                }
            }
        }
        else
        {
            ret = swConnection_buffer_send(conn);
            switch(ret)
            {
            //connection error, close it
            case SW_CLOSE:
                goto close_fd;
            //send continue
            case SW_CONTINUE:
                break;
            //reactor_wait
            case SW_WAIT:
            default:
                return SW_OK;
            }
        }
    }

    //remove EPOLLOUT event
    if (swBuffer_empty(conn->out_buffer))
    {
        reactor->set(reactor, ev->fd, SW_FD_TCP | SW_EVENT_READ);
    }
    return SW_OK;
}

int swReactorThread_onReceive_buffer_check_eof(swReactor *reactor, swEvent *event)
{
    int n, recv_again = SW_FALSE;
    int isEOF = -1;
    int buf_size;

    swServer *serv = SwooleG.serv;
    //swDispatchData send_data;
    swBuffer *buffer;

    swConnection *conn = swServer_connection_get(serv, event->fd);

    volatile swBuffer_trunk *trunk;
    trunk = swConnection_get_in_buffer(conn);

    if (trunk == NULL)
    {
        return swReactorThread_onReceive_no_buffer(reactor, event);
    }

    buffer = conn->in_buffer;

    recv_data:
    buf_size = buffer->trunk_size - trunk->length;

#ifdef SW_USE_EPOLLET
    n = swRead(event->fd,  trunk->data, SW_BUFFER_SIZE);
#else
    //level trigger
    n = recv(event->fd,  trunk->store.ptr + trunk->length, buf_size, 0);
#endif

    swTrace("ReactorThread: recv[len=%d]", n);
    if (n < 0)
    {
        switch (swConnection_error(errno))
        {
        case SW_ERROR:
            swWarn("recv from connection[fd=%d] failed. Error: %s[%d]", conn->fd, strerror(errno), errno);
            return SW_OK;
        case SW_CLOSE:
            goto close_fd;
        default:
            return SW_OK;
        }
    }
    else if (n == 0)
    {
        close_fd:
        swReactorThread_onClose(reactor, event);
        /**
         * skip EPOLLERR
         */
        event->fd = 0;
        return SW_OK;
    }
    else
    {
        //update time
        conn->last_time =  SwooleGS->now;

        //读满buffer了,可能还有数据
        if ((buffer->trunk_size - trunk->length) == n)
        {
            recv_again = SW_TRUE;
        }

        trunk->length += n;
        buffer->length += n;

        //over max length, will discard
        //TODO write to tmp file.
        if (buffer->length > serv->package_max_length)
        {
            swWarn("Package is too big. package_length=%d", buffer->length);
            goto close_fd;
        }

//        printf("buffer[len=%d][n=%d]-----------------\n", trunk->length, n);
        //((char *)trunk->data)[trunk->length] = 0; //for printf
//        printf("buffer-----------------: %s|fd=%d|len=%d\n", (char *) trunk->data, event->fd, trunk->length);

        //EOF_Check
        isEOF = memcmp(trunk->store.ptr + trunk->length - serv->package_eof_len, serv->package_eof, serv->package_eof_len);
//        printf("buffer ok. EOF=%s|Len=%d|RecvEOF=%s|isEOF=%d\n", serv->package_eof, serv->package_eof_len, (char *)trunk->data + trunk->length - serv->package_eof_len, isEOF);

        //received EOF, will send package to worker
        if (isEOF == 0)
        {
            swReactorThread_send_in_buffer(swServer_get_thread(serv, SwooleTG.id), conn);
            return SW_OK;
        }
        else if (recv_again)
        {
            trunk = swBuffer_new_trunk(buffer, SW_TRUNK_DATA, buffer->trunk_size);
            if (trunk)
            {
                goto recv_data;
            }
        }
    }
    return SW_OK;
}

int swReactorThread_onReceive_no_buffer(swReactor *reactor, swEvent *event)
{
    int ret, n;
    swServer *serv = reactor->ptr;
    swFactory *factory = &(serv->factory);
    swDispatchData task;
    swConnection *conn = swServer_connection_get(serv, event->fd);

#ifdef SW_USE_EPOLLET
    n = swRead(event->fd, task.data.data, SW_BUFFER_SIZE);
#else
    //非ET模式会持续通知
    n = swConnection_recv(conn, task.data.data, SW_BUFFER_SIZE, 0);
#endif

    if (n < 0)
    {
        switch (swConnection_error(errno))
        {
        case SW_ERROR:
            swWarn("recv from connection[fd=%d] failed. Error: %s[%d]", event->fd, strerror(errno), errno);
            return SW_OK;
        case SW_CLOSE:
            goto close_fd;
        default:
            return SW_OK;
        }
    }
    //需要检测errno来区分是EAGAIN还是ECONNRESET
    else if (n == 0)
    {
        close_fd:
        swReactorThread_onClose(reactor, event);
        /**
         * skip EPOLLERR
         */
        event->fd = 0;
        return SW_OK;
    }
    else
    {
        swTrace("recv: %s|fd=%d|len=%d\n", task.data.data, event->fd, n);
        //更新最近收包时间
        conn->last_time =  SwooleGS->now;

        //heartbeat ping package
        if (serv->heartbeat_ping_length == n)
        {
            if (serv->heartbeat_pong_length > 0)
            {
                send(event->fd, serv->heartbeat_pong, serv->heartbeat_pong_length, 0);
            }
            return SW_OK;
        }

        task.data.info.fd = event->fd;
        task.data.info.from_id = event->from_id;
        task.data.info.len = n;
        task.data.info.type = SW_EVENT_TCP;
        task.target_worker_id = -1;

#ifdef SW_USE_RINGBUFFER
        if (serv->factory_mode == SW_MODE_PROCESS)
        {
            uint16_t target_worker_id = swServer_worker_schedule(serv, conn->fd);
            swPackage package;

            package.length = task.data.info.len;
            package.data = swReactorThread_alloc(&serv->reactor_threads[SwooleTG.id], package.length);
            task.data.info.type = SW_EVENT_PACKAGE;

            memcpy(package.data, task.data.data, task.data.info.len);
            task.data.info.len = sizeof(package);
            task.target_worker_id = target_worker_id;
            memcpy(task.data.data, &package, sizeof(package));
        }
#endif
        //dispatch to worker process
        ret = factory->dispatch(factory, &task);

#ifdef SW_USE_EPOLLET
        //缓存区还有数据没读完，继续读，EPOLL的ET模式
        if (sw_errno == EAGAIN)
        {
            swWarn("sw_errno == EAGAIN");
            ret = swReactorThread_onReceive_no_buffer(reactor, event);
        }
#endif
        return ret;
    }
    return SW_OK;
}

/**
 * return the package total length
 */
static sw_inline int swReactorThread_get_package_length(swServer *serv, void *data, uint32_t size)
{
    uint16_t length_offset = serv->package_length_offset;
    uint32_t body_length;
    /**
     * no have length field, wait more data
     */
    if (size < length_offset + serv->package_length_size)
    {
        return 0;
    }
    body_length = swoole_unpack(serv->package_length_type, data + length_offset);
    //Length error
    //Protocol length is not legitimate, out of bounds or exceed the allocated length
    if (body_length < 1 || body_length > serv->package_max_length)
    {
        swWarn("invalid package [length=%d].", body_length);
        return SW_ERR;
    }
    //total package length
    return serv->package_body_offset + body_length;
}

int swReactorThread_onReceive_buffer_check_length(swReactor *reactor, swEvent *event)
{
    int n;
    int package_total_length;
    swServer *serv = reactor->ptr;
    swConnection *conn = swServer_connection_get(serv, event->fd);

    char recv_buf[SW_BUFFER_SIZE_BIG];

#ifdef SW_USE_EPOLLET
    n = swRead(event->fd, recv_buf, SW_BUFFER_SIZE_BIG);
#else
    //非ET模式会持续通知
    n = recv(event->fd, recv_buf, SW_BUFFER_SIZE_BIG, 0);
#endif

    if (n < 0)
    {
        error_fd:
        switch (swConnection_error(errno))
        {
        case SW_ERROR:
            swWarn("recv from connection[fd=%d] failed. Error: %s[%d]", conn->fd, strerror(errno), errno);
            return SW_OK;
        case SW_CLOSE:
            goto close_fd;
        default:
            return SW_OK;
        }
    }
    else if (n == 0)
    {
        close_fd:
        swTrace("Close Event.FD=%d|From=%d", event->fd, event->from_id);
        swReactorThread_onClose(reactor, event);
        /**
         * skip EPOLLERR
         */
        event->fd = 0;
        return SW_OK;
    }
    else
    {
        conn->last_time = SwooleGS->now;

        swString tmp_package;
        swString *package;
        void *tmp_ptr = recv_buf;
        uint32_t tmp_n = n;
        uint32_t try_count = 0;

        //new package
        if (conn->object == NULL)
        {
            do_parse_package:
            do
            {
                package_total_length = swReactorThread_get_package_length(serv, (void *)tmp_ptr, (uint32_t) tmp_n);

                //Invalid package, close connection
                if (package_total_length < 0)
                {
                    goto close_fd;
                }
                //no package_length
                else if (package_total_length == 0)
                {
                    char recv_buf_again[SW_BUFFER_SIZE_BIG];
                    memcpy(recv_buf_again, (void *) tmp_ptr, (uint32_t) tmp_n);

                    for(;;)
                    {
                        //前tmp_n个字节存放不完整包头
                        n = recv(event->fd, (void *) recv_buf_again + tmp_n, SW_BUFFER_SIZE_BIG - tmp_n, 0);
                        if (n > 0)
                        {
                            tmp_n += n;
                        }
                        else if (n == 0)
                        {
                            goto close_fd;
                        }
                        else
                        {
                            if (errno == EINTR)
                            {
                                continue;
                            }
                            else if (errno == EAGAIN)
                            {
                                break;
                            }
                            else
                            {
                                goto error_fd;
                            }
                        }

                        try_count++;

                        //连续5次尝试补齐包头,认定为恶意请求
                        if (try_count > 5)
                        {
                            swWarn("no package header, close the connection.");
                            goto close_fd;
                        }
                    }

                    tmp_ptr = recv_buf_again;
                    goto do_parse_package;
                }
                //complete package
                if (package_total_length <= tmp_n)
                {
                    tmp_package.size = package_total_length;
                    tmp_package.length = package_total_length;
                    tmp_package.str = (void *) tmp_ptr;

                    //swoole_dump_bin(buffer.str, 's', buffer.length);
                    swReactorThread_send_string_buffer(swServer_get_thread(serv, SwooleTG.id), conn, &tmp_package);

                    tmp_ptr += package_total_length;
                    tmp_n -= package_total_length;
                    continue;
                }
                //wait more data
                else
                {
                    package = swString_new(package_total_length);
                    if (package == NULL)
                    {
                        return SW_ERR;
                    }
                    memcpy(package->str, (void *) tmp_ptr, (uint32_t) tmp_n);
                    package->length += tmp_n;
                    conn->object = (void *) package;
                    break;
                }
            }
            while(tmp_n > 0);
            return SW_OK;
        }
        //package wait data
        else
        {
            package = conn->object;
            //swTraceLog(40, "wait_data, size=%d, length=%d", buffer->size, buffer->length);

            /**
             * Also on the require_n byte data is complete.
             */
            int require_n = package->size - package->length;

            /**
             * Data is not complete, continue to wait
             */
            if (require_n > n)
            {
                memcpy(package->str + package->length, recv_buf, n);
                package->length += n;
                return SW_OK;
            }
            else
            {
                memcpy(package->str + package->length, recv_buf, require_n);
                package->length += require_n;

                //send buffer to worker pipe
                swReactorThread_send_string_buffer(swServer_get_thread(serv, reactor->id), conn, package);

                //free the buffer memory
                swString_free((swString *) package);
                conn->object = NULL;

                //still have the data, to parse
                if (n - require_n > 0)
                {
                    tmp_n = n - require_n;
                    tmp_ptr = recv_buf + require_n;
                    goto do_parse_package;
                }
            }
        }
    }
    return SW_OK;
}

/**
 * For Http Protocol
 */
int swReactorThread_onReceive_http_request(swReactor *reactor, swEvent *event)
{
    int n = 0;
    int fd = event->fd;
    swServer *serv = reactor->ptr;
    swConnection *conn = swServer_connection_get(serv, fd);

    char *buf;
    int buf_len;
    char recv_buf[SW_BUFFER_SIZE];

    swHttpRequest *request;
    swString tmp_package;

    //new http request
    if (conn->object == NULL)
    {
        request = sw_malloc(sizeof(swHttpRequest));
        bzero(request, sizeof(swHttpRequest));
        conn->object = request;
    }
    else
    {
        request = (swHttpRequest *) conn->object;
    }

    recv_data:
    if (request->method == 0)
    {
        buf = recv_buf;
        buf_len = SW_BUFFER_SIZE;
    }
    else
    {
        buf = request->buffer->str + request->buffer->length;
        buf_len = request->buffer->size - request->buffer->length;
    }

#ifdef SW_USE_EPOLLET
    n = swRead(fd, buf, buf_len);
#else
    //非ET模式会持续通知
    n = recv(fd, buf, buf_len, 0);
#endif

    if (n < 0)
    {
        switch (swConnection_error(errno))
        {
        case SW_ERROR:
            swWarn("recv from connection[fd=%d] failed. Error: %s[%d]", conn->fd, strerror(errno), errno);
            return SW_OK;
        case SW_CLOSE:
            goto close_fd;
        default:
            return SW_OK;
        }
    }
    else if (n == 0)
    {
        close_fd:
        swReactorThread_onClose(reactor, event);
        /**
         * skip EPOLLERR
         */
        event->fd = 0;
        return SW_OK;
    }
    else
    {
        conn->last_time = SwooleGS->now;

        if (request->method == 0)
        {
            bzero(&tmp_package, sizeof(tmp_package));
            tmp_package.str = recv_buf;
            tmp_package.size = SW_BUFFER_SIZE;
            request->buffer = &tmp_package;
        }

        swString *buffer = request->buffer;
        buffer->length += n;

        if (request->method == 0 && swHttpRequest_get_protocol(request) < 0)
        {
            swWarn("get protocol failed.");
            request->buffer = NULL;

#ifdef SW_HTTP_BAD_REQUEST
            if (write(fd, SW_STRL(SW_HTTP_BAD_REQUEST) - 1) < 0)
			{
            	swSysError("write() failed.");
			}
#endif
            goto close_fd;
        }
        //GET
        if (request->method == HTTP_GET)
        {
            if (memcmp(buffer->str + buffer->length - 4, "\r\n\r\n", 4) == 0)
            {
                swReactorThread_send_string_buffer(swServer_get_thread(serv, SwooleTG.id), conn, buffer);
                swHttpRequest_free(request);
            }
            else if (buffer->size == buffer->length)
            {
                swWarn("http header is too long.");
                goto close_fd;
            }
            //wait more data
            else
            {
                wait_more_data:
                if (request->state == 0)
                {
                    request->buffer = swString_dup2(buffer);
                    request->state = SW_WAIT;
                }
                goto recv_data;
            }
        }
        //POST
        else
        {
            if (request->content_length == 0)
            {
                if (swHttpRequest_get_content_length(request) < 0)
                {
                    if (buffer->size == buffer->length)
                    {
                        swWarn("http header is too long.");
                        goto close_fd;
                    }
                    else
                    {
                        goto wait_more_data;
                    }
                }
            }
            else if (request->content_length > serv->package_max_length)
            {
                swWarn("Package length more than the maximum size[%d], Close connection.", serv->package_max_length);
                goto close_fd;
            }

            if (request->content_length == buffer->length - request->header_length)
            {
                swReactorThread_send_string_buffer(swServer_get_thread(serv, SwooleTG.id), conn, buffer);
                swHttpRequest_free(request);
            }
            else
            {
                swTrace("PostWait: request->content_length=%d, buffer->length=%d, request->header_length=%d\n",
                        request->content_length, buffer->length, request->header_length);

                if (request->state > 0)
                {
                    if (request->content_length > buffer->size && swString_extend(buffer, request->content_length) < 0)
                    {
                        swWarn("malloc failed.");
                        return SW_OK;
                    }
                }
                else
                {
                    buffer->size = request->content_length + request->header_length;
                }
                goto wait_more_data;
            }
        }
    }
    return SW_OK;
}

int swReactorThread_create(swServer *serv)
{
    int ret = 0;
    SW_START_SLEEP;

    /**
     * init reactor thread pool
     */
    serv->reactor_threads = SwooleG.memory_pool->alloc(SwooleG.memory_pool, (serv->reactor_num * sizeof(swReactorThread)));
    if (serv->reactor_threads == NULL)
    {
        swError("calloc[reactor_threads] fail.alloc_size=%d", (int )(serv->reactor_num * sizeof(swReactorThread)));
        return SW_ERR;
    }

#ifdef SW_USE_RINGBUFFER
    int i;
    for (i = 0; i < serv->reactor_num; i++)
    {
        serv->reactor_threads[i].buffer_input = swRingBuffer_new(SwooleG.serv->buffer_input_size, 1);
        if (!serv->reactor_threads[i].buffer_input)
        {
            return SW_ERR;
        }
    }
#endif

    serv->connection_list = sw_shm_calloc(serv->max_connection, sizeof(swConnection));
    if (serv->connection_list == NULL)
    {
        swError("calloc[1] failed");
        return SW_ERR;
    }

    //create factry object
    if (serv->factory_mode == SW_MODE_THREAD)
    {
        if (serv->writer_num < 1)
        {
            swError("Fatal Error: serv->writer_num < 1");
            return SW_ERR;
        }
        ret = swFactoryThread_create(&(serv->factory), serv->writer_num);
    }
    else if (serv->factory_mode == SW_MODE_PROCESS)
    {
        if (serv->writer_num < 1 || serv->worker_num < 1)
        {
            swError("Fatal Error: serv->writer_num < 1 or serv->worker_num < 1");
            return SW_ERR;
        }
        ret = swFactoryProcess_create(&(serv->factory), serv->writer_num, serv->worker_num);
    }
    else
    {
        ret = swFactory_create(&(serv->factory));
    }

    if (ret < 0)
    {
        swError("create factory failed");
        return SW_ERR;
    }
    return SW_OK;
}

int swReactorThread_start(swServer *serv, swReactor *main_reactor_ptr)
{
    swThreadParam *param;
    swReactorThread *thread;
    pthread_t pidt;

    int i, ret;
    //listen UDP
    if (serv->have_udp_sock == 1)
    {
        if (swUDPThread_start(serv) < 0)
        {
            swError("udp thread start failed.");
            return SW_ERR;
        }
    }

    //listen TCP
    if (serv->have_tcp_sock == 1)
    {
        //listen server socket
        ret = swServer_listen(serv, main_reactor_ptr);
        if (ret < 0)
        {
            return SW_ERR;
        }
        //create reactor thread
        for (i = 0; i < serv->reactor_num; i++)
        {
            thread = &(serv->reactor_threads[i]);
            param = SwooleG.memory_pool->alloc(SwooleG.memory_pool, sizeof(swThreadParam));
            if (param == NULL)
            {
                swError("malloc failed");
                return SW_ERR;
            }

            param->object = serv;
            param->pti = i;

            if (pthread_create(&pidt, NULL, (void * (*)(void *)) swReactorThread_loop_tcp, (void *) param) < 0)
            {
                swError("pthread_create[tcp_reactor] failed. Error: %s[%d]", strerror(errno), errno);
            }
            thread->thread_id = pidt;
        }
    }

    //timer
    if (SwooleG.timer.fd > 0)
    {
        main_reactor_ptr->add(main_reactor_ptr, SwooleG.timer.fd, SW_FD_TIMER);
    }
    //wait poll thread
    SW_START_SLEEP;
    return SW_OK;
}

/**
 * ReactorThread main Loop
 */
static int swReactorThread_loop_tcp(swThreadParam *param)
{
    swServer *serv = SwooleG.serv;
    int ret;
    int reactor_id = param->pti;

    SwooleTG.factory_lock_target = 0;
    SwooleTG.factory_target_worker = -1;
    SwooleTG.id = reactor_id;
    SwooleTG.type = SW_THREAD_REACTOR;

    swReactorThread *thread = swServer_get_thread(serv, reactor_id);
    swReactor *reactor = &thread->reactor;

    //cpu affinity setting
#if HAVE_CPU_AFFINITY
    if (serv->open_cpu_affinity)
    {
        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);
        CPU_SET(reactor_id % SW_CPU_NUM, &cpu_set);
        if (0 != pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set))
        {
            swWarn("pthread_setaffinity_np set failed");
        }
    }
#endif

    ret = swReactor_auto(reactor, SW_REACTOR_MAXEVENTS);
    if (ret < 0)
    {
        return SW_ERR;
    }

    swSignal_none();

    reactor->ptr = serv;
    reactor->id = reactor_id;

    reactor->onFinish = NULL;
    reactor->onTimeout = NULL;

    reactor->setHandle(reactor, SW_FD_CLOSE, swReactorThread_onClose);
    reactor->setHandle(reactor, SW_FD_UDP, swReactorThread_onPackage);
    reactor->setHandle(reactor, SW_FD_PIPE, swReactorThread_onPipeReceive);
    reactor->setHandle(reactor, SW_FD_PIPE | SW_EVENT_WRITE, swReactorThread_onPipeWrite);
    reactor->setHandle(reactor, SW_FD_TCP | SW_EVENT_WRITE, swReactorThread_onWrite);

    int i = 0, pipe_fd;
    if (serv->ipc_mode != SW_IPC_MSGQUEUE)
    {
        thread->buffer_pipe = swArray_new(serv->workers[serv->worker_num - 1].pipe_master + 1, sizeof(void*), 0);

        for (i = 0; i < serv->worker_num; i++)
        {
            pipe_fd = serv->workers[i].pipe_master;
            //for request
            swBuffer *buffer = swBuffer_new(sizeof(swEventData));
            if (!buffer)
            {
                swWarn("create buffer failed.");
                break;
            }
            if (swArray_store(thread->buffer_pipe, pipe_fd, &buffer, sizeof(buffer)) < 0)
            {
                swWarn("create buffer failed.");
                break;
            }
            //for response
            if (i % serv->reactor_num == reactor_id)
            {
                swSetNonBlock(pipe_fd);
                reactor->add(reactor, pipe_fd, SW_FD_PIPE);
                /**
                 * mapping reactor_id and worker pipe
                 */
                serv->connection_list[pipe_fd].from_id = reactor_id;
            }
        }
    }

    //Thread mode must copy the data.
    //will free after onFinish
    if (serv->open_eof_check)
    {
        reactor->setHandle(reactor, SW_FD_TCP, swReactorThread_onReceive_buffer_check_eof);
    }
    else if (serv->open_length_check)
    {
        reactor->setHandle(reactor, SW_FD_TCP, swReactorThread_onReceive_buffer_check_length);
    }
    else if (serv->open_http_protocol)
    {
        reactor->setHandle(reactor, SW_FD_TCP, swReactorThread_onReceive_http_request);
    }
    else
    {
        reactor->setHandle(reactor, SW_FD_TCP, swReactorThread_onReceive_no_buffer);
    }
    //main loop
    reactor->wait(reactor, NULL);
    //shutdown
    if (serv->ipc_mode != SW_IPC_MSGQUEUE)
    {
        for (i = 0; i < serv->worker_num; i++)
        {
            swWorker *worker = swServer_get_worker(serv, i);
            swBuffer *buffer = *(swBuffer **) swArray_fetch(thread->buffer_pipe, worker->pipe_master);
            swBuffer_free(buffer);
        }
        swArray_free(thread->buffer_pipe);
    }
    reactor->free(reactor);
    pthread_exit(0);
    return SW_OK;
}

static int swUDPThread_start(swServer *serv)
{
    swThreadParam *param;
    pthread_t thread_id;
    swListenList_node *listen_host;

    void * (*thread_loop)(void *);

    LL_FOREACH(serv->listen_list, listen_host)
    {
        param = SwooleG.memory_pool->alloc(SwooleG.memory_pool, sizeof(swThreadParam));
        //UDP
        if (listen_host->type == SW_SOCK_UDP || listen_host->type == SW_SOCK_UDP6 || listen_host->type == SW_SOCK_UNIX_DGRAM)
        {
            serv->connection_list[listen_host->sock].addr.sin_port = listen_host->port;
            serv->connection_list[listen_host->sock].object = listen_host;

            param->object = serv;
            param->pti = listen_host->sock;

            if (listen_host->type == SW_SOCK_UNIX_DGRAM)
            {
                thread_loop = (void * (*)(void *)) swReactorThread_loop_unix_dgram;
            }
            else
            {
                thread_loop = (void * (*)(void *)) swReactorThread_loop_udp;
            }

            if (pthread_create(&thread_id, NULL, thread_loop, (void *) param) < 0)
            {
                swWarn("pthread_create[udp_listener] fail");
                return SW_ERR;
            }
            listen_host->thread_id = thread_id;
        }
    }
    return SW_OK;
}


/**
 * udp listener thread
 */
static int swReactorThread_loop_udp(swThreadParam *param)
{
    int ret;
    socklen_t addrlen;
    swServer *serv = param->object;

    swDispatchData task;
    struct sockaddr_in addr_in;
    addrlen = sizeof(addr_in);

    int sock = param->pti;

    SwooleTG.factory_lock_target = 0;
    SwooleTG.factory_target_worker = -1;
    SwooleTG.id = sock;
    SwooleTG.type = SW_THREAD_UDP;

    swSignal_none();

    //blocking
    swSetBlock(sock);

    bzero(&task.data.info, sizeof(task.data.info));
    task.data.info.from_fd = sock;

    while (SwooleG.running == 1)
    {
        ret = recvfrom(sock, task.data.data, SW_BUFFER_SIZE, 0, (struct sockaddr *)&addr_in, &addrlen);
        if (ret > 0)
        {
            task.data.info.len = ret;
            task.data.info.type = SW_EVENT_UDP;
            //UDP的from_id是PORT，FD是IP
            task.data.info.from_id = ntohs(addr_in.sin_port); //转换字节序
            task.data.info.fd = addr_in.sin_addr.s_addr;
            task.target_worker_id = -1;

            swTrace("recvfrom udp socket.fd=%d|data=%s", sock, task.data.data);
            ret = serv->factory.dispatch(&serv->factory, &task);
            if (ret < 0)
            {
                swWarn("factory->dispatch[udp packet] fail\n");
            }
        }
    }
    pthread_exit(0);
    return 0;
}

static int swReactorThread_send_string_buffer(swReactorThread *thread, swConnection *conn, swString *buffer)
{
    int ret;
    swFactory *factory = SwooleG.factory;
    swDispatchData task;

    task.data.info.fd = conn->fd;
    task.data.info.from_id = conn->from_id;

#ifdef SW_USE_RINGBUFFER
    swServer *serv = SwooleG.serv;
    int target_worker_id = swServer_worker_schedule(serv, conn->fd);
    swPackage package;

    package.length = buffer->length;
    package.data = swReactorThread_alloc(thread, package.length);

    task.data.info.type = SW_EVENT_PACKAGE;
    task.data.info.len = sizeof(package);
    task.target_worker_id = target_worker_id;

    //swoole_dump_bin(package.data, 's', buffer->length);
    memcpy(package.data, buffer->str, buffer->length);
    memcpy(task.data.data, &package, sizeof(package));
    ret = factory->dispatch(factory, &task);
#else

    int send_n = buffer->length;
    task.data.info.type = SW_EVENT_PACKAGE_START;
    task.target_worker_id = -1;

    /**
     * lock target
     */
    SwooleTG.factory_lock_target = 1;

    void *send_ptr = buffer->str;
    do
    {
        if (send_n > SW_BUFFER_SIZE)
        {
            task.data.info.len = SW_BUFFER_SIZE;
            memcpy(task.data.data, send_ptr, SW_BUFFER_SIZE);
        }
        else
        {
            task.data.info.type = SW_EVENT_PACKAGE_END;
            task.data.info.len = send_n;
            memcpy(task.data.data, send_ptr, send_n);
        }

        swTrace("dispatch, type=%d|len=%d\n", task.data.info.type, task.data.info.len);

        ret = factory->dispatch(factory, &task);
        //TODO: 处理数据失败，数据将丢失
        if (ret < 0)
        {
            swWarn("factory->dispatch failed.");
        }
        send_n -= task.data.info.len;
        send_ptr += task.data.info.len;
    }
    while (send_n > 0);

    /**
     * unlock
     */
    SwooleTG.factory_target_worker = -1;
    SwooleTG.factory_lock_target = 0;

#endif
    return ret;
}

static int swReactorThread_send_in_buffer(swReactorThread *thread, swConnection *conn)
{
    swDispatchData task;
    swFactory *factory = SwooleG.factory;

    task.data.info.fd = conn->fd;
    task.data.info.from_id = conn->from_id;

    swBuffer *buffer = conn->in_buffer;
    swBuffer_trunk *trunk = swBuffer_get_trunk(buffer);

#ifdef SW_USE_RINGBUFFER
    swServer *serv = SwooleG.serv;
    uint16_t target_worker_id = swServer_worker_schedule(serv, conn->fd);
    swPackage package;

    package.length = 0;
    package.data = swReactorThread_alloc(thread, buffer->length);

    task.data.info.type = SW_EVENT_PACKAGE;

    while (trunk != NULL)
    {
        task.data.info.len = trunk->length;
        memcpy(package.data + package.length, trunk->store.ptr, trunk->length);
        package.length += trunk->length;

        swBuffer_pop_trunk(buffer, trunk);
        trunk = swBuffer_get_trunk(buffer);
    }
    task.data.info.len = sizeof(package);
    task.target_worker_id = target_worker_id;
    memcpy(task.data.data, &package, sizeof(package));
    //swWarn("[ReactorThread] copy_n=%d", package.length);
    return factory->dispatch(factory, &task);
#else
    int ret;
    task.data.info.type = SW_EVENT_PACKAGE_START;
    task.target_worker_id = -1;

    /**
     * lock target
     */
    SwooleTG.factory_lock_target = 1;

    while (trunk != NULL)
    {
        task.data.info.len = trunk->length;
        memcpy(task.data.data, trunk->store.ptr, task.data.info.len);
        //package end
        if (trunk->next == NULL)
        {
            task.data.info.type = SW_EVENT_PACKAGE_END;
        }
        ret = factory->dispatch(factory, &task);
        //TODO: 处理数据失败，数据将丢失
        if (ret < 0)
        {
            swWarn("factory->dispatch() failed.");
        }
        swBuffer_pop_trunk(buffer, trunk);
        trunk = swBuffer_get_trunk(buffer);

        swTrace("send2worker[trunk_num=%d][type=%d]", buffer->trunk_num, task.data.info.type);
    }
    /**
     * unlock
     */
    SwooleTG.factory_target_worker = -1;
    SwooleTG.factory_lock_target = 0;

#endif
    return SW_OK;
}

/**
 * unix socket dgram thread
 */
static int swReactorThread_loop_unix_dgram(swThreadParam *param)
{
    int n;
    swServer *serv = param->object;
    swDispatchData task;
    struct sockaddr_un addr_un;
    socklen_t addrlen = sizeof(struct sockaddr_un);
    int sock = param->pti;

    uint16_t sun_path_offset;
    uint8_t sun_path_len;

    SwooleTG.factory_lock_target = 0;
    SwooleTG.factory_target_worker = -1;
    SwooleTG.id = param->pti;
    SwooleTG.type = SW_THREAD_UNIX_DGRAM;

    swSignal_none();

    //blocking
    swSetBlock(sock);

    bzero(&task.data.info, sizeof(task.data.info));
    task.data.info.from_fd = sock;
    task.data.info.type = SW_EVENT_UNIX_DGRAM;

    while (SwooleG.running == 1)
    {
        n = recvfrom(sock, task.data.data, SW_BUFFER_SIZE, 0, (struct sockaddr *) &addr_un, &addrlen);
        if (n > 0)
        {
            if (n > SW_BUFFER_SIZE - sizeof(addr_un.sun_path))
            {
                swWarn("Error: unix dgram length must be less than %ld", SW_BUFFER_SIZE - sizeof(addr_un.sun_path));
                continue;
            }

            sun_path_len = strlen(addr_un.sun_path) + 1;
            sun_path_offset = n;
            task.data.info.fd = sun_path_offset;
            task.data.info.len = n + sun_path_len;
            task.target_worker_id = -1;
            memcpy(task.data.data + n, addr_un.sun_path, sun_path_len);
            swTrace("recvfrom udp socket.fd=%d|data=%s", sock, task.data.data);

            n = serv->factory.dispatch(&serv->factory, &task);
            if (n < 0)
            {
                swWarn("factory->dispatch[udp packet] fail\n");
            }
        }
    }
    pthread_exit(0);
    return 0;
}

void swReactorThread_free(swServer *serv)
{
    int i;
    swReactorThread *thread;

    if (SwooleGS->start == 0)
    {
        return;
    }

    if (serv->have_tcp_sock == 1)
    {
        //create reactor thread
        for (i = 0; i < serv->reactor_num; i++)
        {
            thread = &(serv->reactor_threads[i]);
            pthread_cancel(thread->thread_id);
            if (pthread_join(thread->thread_id, NULL))
            {
                swWarn("pthread_join() failed. Error: %s[%d]", strerror(errno), errno);
            }
#ifdef SW_USE_RINGBUFFER
            thread->buffer_input->destroy(thread->buffer_input);
#endif
        }
    }

    if (serv->have_udp_sock == 1)
    {
        swListenList_node *listen_host;
        LL_FOREACH(serv->listen_list, listen_host)
        {
            if (listen_host->type == SW_SOCK_UDP || listen_host->type == SW_SOCK_UDP6
                    || listen_host->type == SW_SOCK_UNIX_DGRAM)
            {
                pthread_cancel(listen_host->thread_id);
                if (pthread_join(listen_host->thread_id, NULL))
                {
                    swWarn("pthread_join() failed. Error: %s[%d]", strerror(errno), errno);
                }
            }
        }
    }
}
