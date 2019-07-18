#ifndef IOCPMODEL_H
#define IOCPMODEL_H

#include <iostream>
#include <winsock2.h>
#include <windows.h>
#include <vector>
#include <assert.h>
#include <string>
#include <mswsock.h>

// 默认IP地址
#define DEFAULT_IP            "127.0.0.1"
#define DEFAULT_PORT          8086

using namespace std;


typedef enum _OPERATION_TYPE    //在完成端口上投递的IO操作类型
{
    ACCEPT_POSTED,
    SEND_POSTED,
    RECV_POSTED,
    NULL_POSTED   //用于初始化的
}OPERATION_TYPE;


//单IO数据结构体定义(用于每一个重叠操作的参数)
typedef struct _PER_IO_CONTEXT
{
    OVERLAPPED     m_Overlapped;                               // 每一个重叠网络操作的重叠结构(针对每一个Socket的每一个操作，都要有一个)
    SOCKET         m_sockAccept;                               // 这个网络操作所使用的Socket
    WSABUF         m_wsaBuf;                                   // WSA类型的缓冲区，用于给重叠操作传参数的
    char           m_szBuffer[4096];                           // 这个是WSABUF里具体存字符的缓冲区,通常设置为1024的倍数
    _OPERATION_TYPE m_OpType;                                   // 标识网络操作的类型(对应上面的枚举)

    // 初始化结构体变量
    _PER_IO_CONTEXT()
    {
        ZeroMemory(&m_Overlapped, sizeof(m_Overlapped));
        ZeroMemory( m_szBuffer,4096 );
        m_sockAccept = INVALID_SOCKET;
        m_wsaBuf.buf = m_szBuffer;
        m_wsaBuf.len = 4096;
        m_OpType     = NULL_POSTED;
    }
    // 释放掉Socket
    ~_PER_IO_CONTEXT()
    {
        if( m_sockAccept!=INVALID_SOCKET )
        {
            closesocket(m_sockAccept);
            m_sockAccept = INVALID_SOCKET;
        }
    }
    // 重置缓冲区内容
    void ResetBuffer()
    {
        ZeroMemory( m_szBuffer,4096 );
    }

} PER_IO_CONTEXT, *PPER_IO_CONTEXT;


//单句柄数据结构体定义(用于每一个完成端口，也就是每一个Socket的参数)

typedef struct _PER_SOCKET_CONTEXT
{
    SOCKET      m_Socket;                                  // 每一个客户端连接的Socket
    SOCKADDR_IN m_ClientAddr;                              // 客户端的地址
    vector<_PER_IO_CONTEXT*> m_arrayIoContext;             // 客户端网络操作的上下文数据，
                                                           // 也就是说对于每一个客户端Socket，是可以在上面同时投递多个IO请求的

    // 初始化
    _PER_SOCKET_CONTEXT()
    {
        m_Socket = INVALID_SOCKET;
        memset(&m_ClientAddr, 0, sizeof(m_ClientAddr));
    }

    // 释放资源
    ~_PER_SOCKET_CONTEXT()
    {
        if( m_Socket!=INVALID_SOCKET )
        {
            closesocket( m_Socket );
            m_Socket = INVALID_SOCKET;
        }
        // 释放掉所有的IO上下文数据
        for( int i=0;i<m_arrayIoContext.size();i++ )
        {
            delete m_arrayIoContext.at(i);
        }
        vector<_PER_IO_CONTEXT*>().swap(m_arrayIoContext);
    }

    // 获取一个新的IoContext
    _PER_IO_CONTEXT* GetNewIoContext()
    {
        _PER_IO_CONTEXT* p = new _PER_IO_CONTEXT;

        m_arrayIoContext.push_back(p);

        return p;
    }

    // 从数组中移除一个指定的IoContext
    void RemoveContext( _PER_IO_CONTEXT* pContext )
    {
        assert( pContext!=NULL );

        for( int i=0;i<m_arrayIoContext.size();i++ )
        {
            if( pContext==m_arrayIoContext.at(i) )
            {
                delete pContext;
                pContext = NULL;
                vector<_PER_IO_CONTEXT*>::iterator iter=m_arrayIoContext.begin();
                m_arrayIoContext.erase(iter+i);
                break;
            }
        }
    }

} PER_SOCKET_CONTEXT, *PPER_SOCKET_CONTEXT;  //这里的 PER_IO_CONTEXT 相当于 _PER_IO_CONTEXT 的别名 ， 再次定义该结构类型 就要使用 PER_IO_CONTEXT



class IOCPMODEL;
//设定工作者线程参数
typedef struct _tagThreadParams_WORKER
{
    IOCPMODEL* pIOCPModel;                                   // 类指针，用于调用类中的函数
    int         nThreadNo;                                    // 线程编号

} THREADPARAMS_WORKER,*PTHREADPARAM_WORKER;


//进行IOCPModel类的定义
class IOCPMODEL
{
public:

    IOCPMODEL();
    ~IOCPMODEL();
    //启动服务器
    bool Start();
    
    //停止服务器
    void Stop();
    
    //加在Socket库
    bool LoadSocket();
    
    //卸载Socket库
    void UnloadSocket() { WSACleanup(); }
    
    //获得本机IP
    string GetLocalIP();
    
    //设置监听端口
    void SetListenPort(const int &port) { m_Port=port; }

    //Get numbers of socket
    int GetNumOfSocket();


    //Gui Fan shu chu
    void normalExitOutput(PER_SOCKET_CONTEXT * temp);
    
protected:
    
    //初始化IOCP
    bool InitializeIOCP();
    
    //初始化Socket库
    bool InitializeSocket();
    
    //最后释放资源
    void DeInitializeSore();
    
    //投递 AcceptEx 请求
    bool PostAccept(PER_IO_CONTEXT* pAcceptIoContext);
    
    //投递接收数据请求
    bool PostRecv(PER_IO_CONTEXT* pIoContext);
    
    //在有客户端链接的时候，进行处理
    bool DoAccept(PER_SOCKET_CONTEXT* pSocketContext,PER_IO_CONTEXT* pIoContext);
    
    //有数据到达的时候  进行处理
    bool DoRecv(PER_SOCKET_CONTEXT* pSocketContext,PER_IO_CONTEXT* pIoContext);
    
    //将客户端的信息存到数组中
    void AddToContextList(PER_SOCKET_CONTEXT* pHandleData);

    // 将客户端的信息从数组中移除
    void _RemoveContext( PER_SOCKET_CONTEXT *pSocketContext );
    
    // 清空客户端信息
    void ClearContextList();

    // 将句柄绑定到完成端口中
    bool AssociateWithIOCP( PER_SOCKET_CONTEXT *pContext);

    // 处理完成端口上的错误
    bool HandleError( PER_SOCKET_CONTEXT *pContext,const DWORD& dwErr );

    //线程函数，为IOCP请求服务的工作者线程
    static DWORD WINAPI WorkerThread(LPVOID lpParam);

    //获得本机的处理器数量
    int GetNumOfProcessers();

    //判断客户端Socket是否已经断开
    bool IsSocketAlive(SOCKET s);

private:

    HANDLE                   m_hShutdownEvent;              //用来通知线程系统退出的事件，为了能够更好的退出线程

    HANDLE                   m_hIOCompletionPort;           //完成端口的句柄

    HANDLE*                  m_hWorkerThread;               //工作者线程句柄

    int                      m_nThread;                     //生成的工作者线程数量

    string                   m_strIP;                          //服务器的IP地址

    int                      m_Port;                        //服务器的监听端口

    CRITICAL_SECTION         m_csContextList;               // 用于Worker线程同步的互斥量

    vector<PER_SOCKET_CONTEXT*>  m_arrayClientContext;          // 客户端Socket的Context信息

    PER_SOCKET_CONTEXT*          m_pListenContext;              // 用于监听的Socket的Context信息

    LPFN_ACCEPTEX                m_lpfnAcceptEx;                // AcceptEx 和 GetAcceptExSockaddrs 的函数指针，用于调用这两个扩展函数
    LPFN_GETACCEPTEXSOCKADDRS    m_lpfnGetAcceptExSockAddrs;

};


#endif // IOCPMODEL_H


















