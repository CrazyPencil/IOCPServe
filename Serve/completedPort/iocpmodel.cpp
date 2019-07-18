#include "iocpmodel.h"


// 传递给Worker线程的退出信号
#define  EXIT_CODE       NULL

bool InputOnConsole=true;

// 每一个处理器上产生多少个线程(为了最大限度的提升服务器性能，详见配套文档)
const int WORKER_THREADS_PER_PROCESSOR =2;

// 同时投递的Accept请求的数量(这个要根据实际的情况灵活设置)
const int MAX_POST_ACCEPT=10;

//释放一个套接字句柄
void RELEASE_SOCKET(SOCKET x)
{
    if(x !=INVALID_SOCKET)
    {
        closesocket(x);
        x=INVALID_SOCKET;
    }
}

//释放一个事件句柄
void RELEASE_HANDLE(HANDLE x)
{
    if(x !=NULL && x!=INVALID_HANDLE_VALUE)
    {
        CloseHandle(x);
        x=NULL;
    }
}

//释放一个指针
#define RELEASE(x){ if(x !=NULL ) { delete x;x=NULL;}}



IOCPMODEL::IOCPMODEL():
                      m_nThread(0),
                      m_hShutdownEvent(NULL),
                      m_hIOCompletionPort(NULL),
                      m_hWorkerThread(NULL),
                      m_strIP(DEFAULT_IP),
                      m_Port(DEFAULT_PORT),
                      m_lpfnAcceptEx( NULL ),
                      m_pListenContext( NULL )
{
}

IOCPMODEL::~IOCPMODEL()
{
    // 确保资源彻底释放
     this->Stop();
}



//工作者线程函数,当完成端口上出现了完成数据包，取出来进行处理的函数
DWORD WINAPI IOCPMODEL::WorkerThread(LPVOID lpParam)
{

     THREADPARAMS_WORKER* pParam=(THREADPARAMS_WORKER*) lpParam;
     IOCPMODEL* pIOCPModel = (IOCPMODEL*)pParam->pIOCPModel;
     int nThreadNo=(int)pParam->nThreadNo;

     OVERLAPPED           *pOverlapped = NULL;
     PER_SOCKET_CONTEXT   *pSocketContext = NULL;
     DWORD                dwBytesTransfered = 0;


     //循环处理
     while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hShutdownEvent, 0))
     {


         BOOL bReturn = GetQueuedCompletionStatus(
             pIOCPModel->m_hIOCompletionPort,
             &dwBytesTransfered,
             (PULONG_PTR)&pSocketContext,
             &pOverlapped,
             INFINITE);

         // 如果收到的是退出标志，则直接退出
         if ( EXIT_CODE==(DWORD)pSocketContext )
         {
             break;
         }
         // 判断是否出现了错误
         if( !bReturn )
         {
             DWORD dwErr = GetLastError();

             // 显示一下提示信息
             if( !pIOCPModel->HandleError( pSocketContext,dwErr ) )
             {
                 break;
             }

             continue;
         }
         else
         {
             // 读取传入的参数
             PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);

             // 判断是否有客户端断开了
             if((0 == dwBytesTransfered) && ( RECV_POSTED==pIoContext->m_OpType || SEND_POSTED==pIoContext->m_OpType))
             {
                 pIOCPModel->normalExitOutput(pSocketContext);
                 //cout<<"客户端 "<<inet_ntoa(pSocketContext->m_ClientAddr.sin_addr)<<": "<<ntohs(pSocketContext->m_ClientAddr.sin_port)<<" 断开连接."<<endl;

                 // 释放掉对应的资源
                 pIOCPModel->_RemoveContext( pSocketContext );

                 continue;
             }
             else
             {
                 switch( pIoContext->m_OpType )
                 {
                      // Accept
                 case ACCEPT_POSTED:
                     {

                         // 为了增加代码可读性，这里用专门的_DoAccept函数进行处理连入请求
                         pIOCPModel->DoAccept( pSocketContext, pIoContext );
                         cout<<pIOCPModel->GetNumOfSocket()<<endl;


                     }
                     break;

                     // RECV
                 case RECV_POSTED:
                     {
                         // 为了增加代码可读性，这里用专门的_DoRecv函数进行处理接收请求
                         pIOCPModel->DoRecv( pSocketContext,pIoContext );

                     }
                     break;

                     // SEND
                     // 这里略过不写了，要不代码太多了，不容易理解，Send操作相对来讲简单一些
                 case SEND_POSTED:
                     {

                     }
                     break;
                 default:
                     // 不应该执行到这里
                     cout<<"_WorkThread中的 pIoContext->m_OpType 参数异常."<<endl;
                     break;
                 }
             }
         }
     }

     cout<<"工作者线程 "<<nThreadNo<<" 号退出."<<endl;

     // 释放线程参数
     RELEASE(lpParam);

     return 0;

}



//系统的初始化和终止

//初始化Socket
bool IOCPMODEL::InitializeSocket()
{
    //服务器地址信息结构
    sockaddr_in ServerAddress;

    //生成用于监听的Socket的信息
    m_pListenContext=new PER_SOCKET_CONTEXT;

    //设置重叠IO,必须使用WSASocket()
    m_pListenContext->m_Socket = WSASocket(AF_INET,SOCK_STREAM,0,NULL,0,WSA_FLAG_OVERLAPPED);
    if(m_pListenContext->m_Socket==INVALID_SOCKET)
    {
        cout<<"初始化Socket失败，错误代码： "<<WSAGetLastError()<<endl;
        return false;
    }
    else
        cout<<"初始化Socket成功!"<<endl;

    //将Socket绑定在完成端口上
    if(NULL==CreateIoCompletionPort((HANDLE)m_pListenContext->m_Socket,m_hIOCompletionPort,(DWORD)m_pListenContext,0))//第三个参数是自己定义的套接字信息结构，
    {                                                                                                                 //这样可以到Worker线程里面也可以使用，相当于传参
        cout<<"绑定 Listen Socket 至完成端口失败! 错误代码："<<WSAGetLastError()<<endl;
        RELEASE_SOCKET(m_pListenContext->m_Socket);
        return false;
    }
    else
        cout<<"Listen Socket 绑定完成端口成功!"<<endl;
    this->GetLocalIP();
    //开始填充服务器地址信息
    ZeroMemory((sockaddr*)&ServerAddress,sizeof(ServerAddress));
    ServerAddress.sin_family=AF_INET;
    ServerAddress.sin_addr.s_addr = inet_addr(m_strIP.c_str());
    ServerAddress.sin_port=htons(m_Port);

    //绑定地址和端口
    if(SOCKET_ERROR==bind(m_pListenContext->m_Socket,(sockaddr*)&ServerAddress,sizeof(ServerAddress)))
    {
        cout<<"bind()函数执行失败!"<<endl;
        return false;
    }
    else
        cout<<"bind()函数执行成功!"<<endl;

    //开始进行监听
    if(SOCKET_ERROR==listen(m_pListenContext->m_Socket,SOMAXCONN))
    {
        cout<<"listen()函数执行失败!"<<endl;
        return false;
    }

    //现在使用AcceptEx()函数  和  GetAcceptExSockAddrs函数，首先需要获得指针
    GUID GuidAcceptEx = WSAID_ACCEPTEX;
    GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;   //这两个变量用于导出函数指针
    DWORD dwBytes=0;
    if(SOCKET_ERROR==WSAIoctl(m_pListenContext->m_Socket,SIO_GET_EXTENSION_FUNCTION_POINTER,&GuidAcceptEx,sizeof(GuidAcceptEx),
                              &m_lpfnAcceptEx,sizeof(m_lpfnAcceptEx),&dwBytes,NULL,NULL))

    {
        cout<<"未能获得AcceptEx()函数指针! 错误代码："<<WSAGetLastError()<<endl;
        DeInitializeSore();
        return false;
    }

    if(SOCKET_ERROR==WSAIoctl(m_pListenContext->m_Socket,SIO_GET_EXTENSION_FUNCTION_POINTER,&GuidGetAcceptExSockAddrs,sizeof(GuidGetAcceptExSockAddrs),
                              &m_lpfnGetAcceptExSockAddrs,sizeof(m_lpfnGetAcceptExSockAddrs),&dwBytes,NULL,NULL))

    {
        cout<<"未能获得GetAcceptExSockAddrs函数指针! 错误代码："<<WSAGetLastError()<<endl;
        DeInitializeSore();
        return false;
    }

    //获得函数指针之后，开始为AcceptEx函数准备参数，投递AcceptEx IO请求
    for(int i=0;i<200;++i)
    {
        PER_IO_CONTEXT* pAcceptIoContext = m_pListenContext->GetNewIoContext(); //GetNewIoContext()函数只是分配一个空间 放进vector里面 还没有存入信息
        if(false==this->PostAccept(pAcceptIoContext))
        {
           m_pListenContext->RemoveContext(pAcceptIoContext);
           return false;
        }
    }
    cout<<"投递 30 个AcceptEx请求完毕!"<<endl;
    return true;
}




//最后释放所有资源
void IOCPMODEL::DeInitializeSore()
{
    //删除客户端互斥变量
    DeleteCriticalSection(&m_csContextList);

    //关闭系统推出事件句柄
    RELEASE_HANDLE(m_hShutdownEvent);

    //释放工作者线程句柄
    for(int i=0;i<m_nThread;++i)
    {
        RELEASE_HANDLE(m_hWorkerThread[i]);
    }
    RELEASE(m_hWorkerThread); //释放掉工作者线程指针

    RELEASE_HANDLE(m_hIOCompletionPort);//释放掉完成端口句柄

    RELEASE(m_pListenContext); //释放掉监听套接字指针

    cout<<"资源释放完毕!"<<endl;
}

//投递AcceptEx请求
bool IOCPMODEL::PostAccept(PER_IO_CONTEXT *pAcceptIoContext)
{
    assert(INVALID_SOCKET!=m_pListenContext->m_Socket);

    //准备参数
    DWORD dwBytes=0;
    pAcceptIoContext->m_OpType=ACCEPT_POSTED;
    WSABUF *P_wbuf = &pAcceptIoContext->m_wsaBuf;
    OVERLAPPED *p_ol=&pAcceptIoContext->m_Overlapped;

    //为以后新连入的客户端先准备好Socket(这是与传统的Accept最大的区别)
    pAcceptIoContext->m_sockAccept=WSASocket(AF_INET,SOCK_STREAM,IPPROTO_TCP,NULL,0,WSA_FLAG_OVERLAPPED);
    if(INVALID_SOCKET == pAcceptIoContext->m_sockAccept)
    {
        cout<<"提前创建用于AcceptEx()的Socket失败! 错误代码："<<WSAGetLastError()<<endl;
        return false;
    }

    //投递AcceptEx，调用该函数
    if(false==m_lpfnAcceptEx( m_pListenContext->m_Socket,pAcceptIoContext->m_sockAccept,P_wbuf->buf,P_wbuf->len-((sizeof(SOCKADDR_IN)+16)*2),
                              sizeof(SOCKADDR_IN)+16,sizeof(SOCKADDR_IN)+16,&dwBytes,p_ol))
    {
        if(WSA_IO_PENDING != WSAGetLastError())
        {
           cout<<"投递AcceptEx请求失败! 错区代码："<<WSAGetLastError()<<endl;
           return false;
        }
    }
    return true;
}

//// 投递接收数据请求
bool IOCPMODEL::PostRecv(PER_IO_CONTEXT *pIoContext)
{
    // 初始化变量
        DWORD dwFlags = 0;                                //这个函数我们需要准备好存放数据的缓冲区就好了
        DWORD dwBytes = 0;
        WSABUF *p_wbuf   = &pIoContext->m_wsaBuf;
        OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

        pIoContext->ResetBuffer();
        pIoContext->m_OpType = RECV_POSTED;

        // 初始化完成后，，投递WSARecv请求
        int nBytesRecv = WSARecv( pIoContext->m_sockAccept, p_wbuf, 1, &dwBytes, &dwFlags, p_ol, NULL );

        // 如果返回值错误，并且错误的代码并非是Pending的话，那就说明这个重叠请求失败了
        if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
        {
            cout<<"投递第一个WSARecv失败！"<<endl;
            return false;
        }
        return true;
}


//当有客户端连入时  进行处理。   传入的是ListenSocket的Context，我们需要复制一份出来给新连入的Socket用   原来的Context还是要在上面继续投递下一个Accept请求
bool IOCPMODEL::DoAccept(PER_SOCKET_CONTEXT *pSocketContext, PER_IO_CONTEXT *pIoContext)
{
    //首先获得客户端的地址信息   利用GetAcceptExSockAddrs函数   不但取得客户端地址信息   还能获得客户端发来的第一组数据
    SOCKADDR_IN* ClientAddr = NULL;
    SOCKADDR_IN* LocalAddr = NULL;
    int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);

    this->m_lpfnGetAcceptExSockAddrs(pIoContext->m_wsaBuf.buf,pIoContext->m_wsaBuf.len-((sizeof(SOCKADDR_IN)+16)*2),
                                     sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16,(LPSOCKADDR*)&LocalAddr,&localLen,
                                     (LPSOCKADDR*)&ClientAddr,&remoteLen);
   // cout<<"客户端 "<<inet_ntoa(ClientAddr->sin_addr)<<": "<<ntohs(ClientAddr->sin_port)<<" 连入!"<<endl;
  //  cout<<"客户端 "<<inet_ntoa(ClientAddr->sin_addr)<<": "<<ntohs(ClientAddr->sin_port)<<" 的信息是："<<pIoContext->m_wsaBuf.buf<<endl;



    //这里需要注意，这里传入的这个是ListenSocket上的Context，这个Context我们还需要用于监听下一个连接
    // 所以我还得要将ListenSocket上的Context复制出来一份为新连入的Socket新建一个SocketContext
    PER_SOCKET_CONTEXT* pNewSocketContext = new PER_SOCKET_CONTEXT;
    pNewSocketContext->m_Socket           = pIoContext->m_sockAccept;
    memcpy(&(pNewSocketContext->m_ClientAddr), ClientAddr, sizeof(SOCKADDR_IN));
    // 参数设置完毕，将这个Socket和完成端口绑定(这也是一个关键步骤)
        if( false==this->AssociateWithIOCP( pNewSocketContext ) )
        {
            RELEASE( pNewSocketContext );
            return false;
        }

        // 3. 继续，建立其下的IoContext，用于在这个Socket上投递第一个Recv数据请求
        PER_IO_CONTEXT* pNewIoContext = pNewSocketContext->GetNewIoContext();
        pNewIoContext->m_OpType       = RECV_POSTED;
        pNewIoContext->m_sockAccept   = pNewSocketContext->m_Socket;
        // 如果Buffer需要保留，就自己拷贝一份出来
        //memcpy( pNewIoContext->m_szBuffer,pIoContext->m_szBuffer,MAX_BUFFER_LEN );

        // 绑定完毕之后，就可以开始在这个Socket上投递完成请求了
        if( false==this->PostRecv( pNewIoContext) )
        {
            pNewSocketContext->RemoveContext( pNewIoContext );
            return false;
        }

        // 4. 如果投递成功，那么就把这个有效的客户端信息，加入到ContextList中去(需要统一管理，方便释放资源)
        this->AddToContextList( pNewSocketContext );


        // 5. 使用完毕之后，把Listen Socket的那个IoContext重置，然后准备投递新的AcceptEx
        pIoContext->ResetBuffer();
        return this->PostAccept( pIoContext );


}

//在有接收的数据到达的时候，进行处理
bool IOCPMODEL::DoRecv(PER_SOCKET_CONTEXT *pSocketContext, PER_IO_CONTEXT *pIoContext)
{

    // 先把上一次的数据显示出现，然后就重置状态，发出下一个Recv请求
        SOCKADDR_IN* ClientAddr = &pSocketContext->m_ClientAddr;
       // cout<<"收到"<<inet_ntoa(ClientAddr->sin_addr)<<": "<<ntohs(ClientAddr->sin_port)<< "信息："<<pIoContext->m_wsaBuf.buf<<endl;

        // 然后开始投递下一个WSARecv请求
        return PostRecv( pIoContext );
}

//将客户端的相关信息存储到数组中
void IOCPMODEL::AddToContextList(PER_SOCKET_CONTEXT *pHandleData)
{
    EnterCriticalSection(&m_csContextList);

    m_arrayClientContext.push_back(pHandleData);

    LeaveCriticalSection(&m_csContextList);
}


//移除某个特定的Context
void IOCPMODEL::_RemoveContext(PER_SOCKET_CONTEXT *pSocketContext)
{
    EnterCriticalSection(&m_csContextList);

        for( int i=0;i<m_arrayClientContext.size();i++ )
        {
            if( pSocketContext==m_arrayClientContext.at(i) )
            {
                RELEASE( pSocketContext );
                vector<PER_SOCKET_CONTEXT*>::iterator iter=m_arrayClientContext.begin();
                m_arrayClientContext.erase(iter+i);
                break;
            }
        }
        LeaveCriticalSection(&m_csContextList);
}


//清空客户端信息
void IOCPMODEL::ClearContextList()
{
    EnterCriticalSection(&m_csContextList);

        for( int i=0;i<m_arrayClientContext.size();i++ )
        {
            delete m_arrayClientContext.at(i);
        }

       // vector<PER_SOCKET_CONTEXT*>().swap(m_arrayClientContext);

    LeaveCriticalSection(&m_csContextList);
}



//将句柄(Socket)绑定到完成端口中
bool IOCPMODEL::AssociateWithIOCP(PER_SOCKET_CONTEXT *pContext)
{
    // 将用于和客户端通信的SOCKET绑定到完成端口中
        HANDLE hTemp = CreateIoCompletionPort((HANDLE)pContext->m_Socket, m_hIOCompletionPort, (DWORD)pContext, 0);

        if (NULL == hTemp)
        {
            cout<<"执行CreateIoCompletionPort()出现错误.错误代码："<<GetLastError()<<endl;
            return false;
        }

        return true;
}

//显示并处理完成端口上的错误
bool IOCPMODEL::HandleError(PER_SOCKET_CONTEXT *pContext, const DWORD &dwErr)
{
    // 如果是超时了，就再继续等吧
        if(WAIT_TIMEOUT == dwErr)
        {
            // 确认客户端是否还活着...
            if( !IsSocketAlive( pContext->m_Socket) )
            {
                cout<<"检测到客户端异常退出!"<<endl;;
                this->_RemoveContext( pContext );
                return true;
            }
            else
            {
                cout<<"网络操作超时! 重试中..."<<endl;
                return true;
            }
        }

        // 可能是客户端异常退出了
        else if( ERROR_NETNAME_DELETED==dwErr )
        {
            cout<<"检测到客户端异常退出！"<<endl;
            this->_RemoveContext( pContext );
            return true;
        }

        else
        {
            cout<<"完成端口操作出现错误，线程退出。错误代码："<<dwErr<<endl;
            return false;
        }
}



//启动服务器
bool IOCPMODEL::Start()
{
     //建立一个系统线程互斥变量
     InitializeCriticalSection(&m_csContextList);

     //建立系统退出的事件通知
     m_hShutdownEvent=CreateEvent(NULL,TRUE,FALSE,NULL);

     //初始化IOCP
     if(false==InitializeIOCP())
     {
         cout<<"IOCP初始化失败!"<<endl;
         return false;
     }
     else
         cout<<"IOCP初始化完成!!"<<endl;

     //初始化Socket
     if(false==InitializeSocket())
     {
         cout<<"Listen Socket初始化失败!"<<endl;
         return false;
     }
     else
         cout<<"Listen Socket初始化完成!!"<<endl;

     cout<<"系统准备就绪，等候链接!"<<endl;
     return true;
}

//开始发送系统退出消息，退出完成端口和线程资源
void IOCPMODEL::Stop()
{
    if( m_pListenContext!=NULL && m_pListenContext->m_Socket!=INVALID_SOCKET )
        {
            // 激活关闭消息通知
            SetEvent(m_hShutdownEvent);

            for (int i = 0; i < m_nThread; i++)
            {
                // 通知所有的完成端口操作退出
                PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD)EXIT_CODE, NULL);
            }

            // 等待所有的客户端资源退出
            WaitForMultipleObjects(m_nThread, m_hWorkerThread, TRUE, INFINITE);

            // 清除客户端列表信息
            this->ClearContextList();

            // 释放其他资源
            this-> DeInitializeSore();

            cout<<"停止监听"<<endl;
        }
}


//加载winsock2
bool IOCPMODEL::LoadSocket()
{
    WSADATA wsadata;
    int result=WSAStartup(MAKEWORD(2,2),&wsadata);
    if(NO_ERROR!=result)
    {
        cout<<"初始化Winsock2.2失败!"<<endl;
        return false;
    }
    else
        cout<<"初始化Winsock2.2成功!"<<endl;
    return true;
}


//获得本机的IP地址
string IOCPMODEL::GetLocalIP()
{
    // 获得本机主机名
        char hostname[MAX_PATH] = {0};
        gethostname(hostname,MAX_PATH);
        struct hostent FAR* lpHostEnt = gethostbyname(hostname);
        if(lpHostEnt == NULL)
        {
            return DEFAULT_IP;
        }

        // 取得IP地址列表中的第一个为返回的IP(因为一台主机可能会绑定多个IP)
        LPSTR lpAddr = lpHostEnt->h_addr_list[0];

        // 将IP地址转化成字符串形式
        struct in_addr inAddr;
        memmove(&inAddr,lpAddr,4);
        m_strIP = string( inet_ntoa(inAddr) );

        return m_strIP;
}

//获得客户端socket数量
int IOCPMODEL::GetNumOfSocket()
{
    EnterCriticalSection(&m_csContextList);
    int temp=m_arrayClientContext.size();
    LeaveCriticalSection(&m_csContextList);
    return temp;
}




void IOCPMODEL::normalExitOutput(PER_SOCKET_CONTEXT   * temp)
{
    EnterCriticalSection(&m_csContextList);
    cout<<"客户端 "<<inet_ntoa(temp->m_ClientAddr.sin_addr)<<": "<<ntohs(temp->m_ClientAddr.sin_port)<<" 断开连接."<<endl;
    LeaveCriticalSection(&m_csContextList);
}


//初始化完成端口
bool IOCPMODEL::InitializeIOCP()
{
    //建立第一个完成端口
    m_hIOCompletionPort=CreateIoCompletionPort(INVALID_HANDLE_VALUE,NULL,0,0);
    if(NULL==m_hIOCompletionPort)
    {
        cout<<"建立完成端口失败!错误代码为："<<WSAGetLastError()<<endl;
        return false;
    }
    else
        cout<<"建立完成端口成功!"<<endl;

    //根据处理器个数开辟线程
    m_nThread=GetNumOfProcessers()*2;
    m_hWorkerThread =new HANDLE[m_nThread];  //创建线程数组  别忘记释放
    DWORD nThreadID;//线程id
    for(int i=0;i<m_nThread;++i)
    {
        //自己定义的线程参数
        THREADPARAMS_WORKER* pThreadParams=new THREADPARAMS_WORKER;
        pThreadParams->pIOCPModel=this;
        pThreadParams->nThreadNo=i+1;
        m_hWorkerThread[i]=CreateThread(0,0,WorkerThread,(void*)pThreadParams,0,&nThreadID);
    }

    cout<<"创建工作者线程 "<<m_nThread<<" 个! 并且完全启动!"<<endl;
    return TRUE;
}


//获得系统处理器个数
int IOCPMODEL::GetNumOfProcessers()
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
}

//判断客户端Socket是否已经断开，否则在一个无效的Socket上投递WSARecv操作会出现异常
bool IOCPMODEL::IsSocketAlive(SOCKET s)
{
    int nByteSent=send(s,"",0,0);
    if (-1 == nByteSent) return false;
    return true;
}












