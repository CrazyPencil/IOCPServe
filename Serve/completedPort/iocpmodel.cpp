#include "iocpmodel.h"


// ���ݸ�Worker�̵߳��˳��ź�
#define  EXIT_CODE       NULL

bool InputOnConsole=true;

// ÿһ���������ϲ������ٸ��߳�(Ϊ������޶ȵ��������������ܣ���������ĵ�)
const int WORKER_THREADS_PER_PROCESSOR =2;

// ͬʱͶ�ݵ�Accept���������(���Ҫ����ʵ�ʵ�����������)
const int MAX_POST_ACCEPT=10;

//�ͷ�һ���׽��־��
void RELEASE_SOCKET(SOCKET x)
{
    if(x !=INVALID_SOCKET)
    {
        closesocket(x);
        x=INVALID_SOCKET;
    }
}

//�ͷ�һ���¼����
void RELEASE_HANDLE(HANDLE x)
{
    if(x !=NULL && x!=INVALID_HANDLE_VALUE)
    {
        CloseHandle(x);
        x=NULL;
    }
}

//�ͷ�һ��ָ��
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
    // ȷ����Դ�����ͷ�
     this->Stop();
}



//�������̺߳���,����ɶ˿��ϳ�����������ݰ���ȡ�������д���ĺ���
DWORD WINAPI IOCPMODEL::WorkerThread(LPVOID lpParam)
{

     THREADPARAMS_WORKER* pParam=(THREADPARAMS_WORKER*) lpParam;
     IOCPMODEL* pIOCPModel = (IOCPMODEL*)pParam->pIOCPModel;
     int nThreadNo=(int)pParam->nThreadNo;

     OVERLAPPED           *pOverlapped = NULL;
     PER_SOCKET_CONTEXT   *pSocketContext = NULL;
     DWORD                dwBytesTransfered = 0;


     //ѭ������
     while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hShutdownEvent, 0))
     {


         BOOL bReturn = GetQueuedCompletionStatus(
             pIOCPModel->m_hIOCompletionPort,
             &dwBytesTransfered,
             (PULONG_PTR)&pSocketContext,
             &pOverlapped,
             INFINITE);

         // ����յ������˳���־����ֱ���˳�
         if ( EXIT_CODE==(DWORD)pSocketContext )
         {
             break;
         }
         // �ж��Ƿ�����˴���
         if( !bReturn )
         {
             DWORD dwErr = GetLastError();

             // ��ʾһ����ʾ��Ϣ
             if( !pIOCPModel->HandleError( pSocketContext,dwErr ) )
             {
                 break;
             }

             continue;
         }
         else
         {
             // ��ȡ����Ĳ���
             PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);

             // �ж��Ƿ��пͻ��˶Ͽ���
             if((0 == dwBytesTransfered) && ( RECV_POSTED==pIoContext->m_OpType || SEND_POSTED==pIoContext->m_OpType))
             {
                 pIOCPModel->normalExitOutput(pSocketContext);
                 //cout<<"�ͻ��� "<<inet_ntoa(pSocketContext->m_ClientAddr.sin_addr)<<": "<<ntohs(pSocketContext->m_ClientAddr.sin_port)<<" �Ͽ�����."<<endl;

                 // �ͷŵ���Ӧ����Դ
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

                         // Ϊ�����Ӵ���ɶ��ԣ�������ר�ŵ�_DoAccept�������д�����������
                         pIOCPModel->DoAccept( pSocketContext, pIoContext );
                         cout<<pIOCPModel->GetNumOfSocket()<<endl;


                     }
                     break;

                     // RECV
                 case RECV_POSTED:
                     {
                         // Ϊ�����Ӵ���ɶ��ԣ�������ר�ŵ�_DoRecv�������д����������
                         pIOCPModel->DoRecv( pSocketContext,pIoContext );

                     }
                     break;

                     // SEND
                     // �����Թ���д�ˣ�Ҫ������̫���ˣ���������⣬Send�������������һЩ
                 case SEND_POSTED:
                     {

                     }
                     break;
                 default:
                     // ��Ӧ��ִ�е�����
                     cout<<"_WorkThread�е� pIoContext->m_OpType �����쳣."<<endl;
                     break;
                 }
             }
         }
     }

     cout<<"�������߳� "<<nThreadNo<<" ���˳�."<<endl;

     // �ͷ��̲߳���
     RELEASE(lpParam);

     return 0;

}



//ϵͳ�ĳ�ʼ������ֹ

//��ʼ��Socket
bool IOCPMODEL::InitializeSocket()
{
    //��������ַ��Ϣ�ṹ
    sockaddr_in ServerAddress;

    //�������ڼ�����Socket����Ϣ
    m_pListenContext=new PER_SOCKET_CONTEXT;

    //�����ص�IO,����ʹ��WSASocket()
    m_pListenContext->m_Socket = WSASocket(AF_INET,SOCK_STREAM,0,NULL,0,WSA_FLAG_OVERLAPPED);
    if(m_pListenContext->m_Socket==INVALID_SOCKET)
    {
        cout<<"��ʼ��Socketʧ�ܣ�������룺 "<<WSAGetLastError()<<endl;
        return false;
    }
    else
        cout<<"��ʼ��Socket�ɹ�!"<<endl;

    //��Socket������ɶ˿���
    if(NULL==CreateIoCompletionPort((HANDLE)m_pListenContext->m_Socket,m_hIOCompletionPort,(DWORD)m_pListenContext,0))//�������������Լ�������׽�����Ϣ�ṹ��
    {                                                                                                                 //�������Ե�Worker�߳�����Ҳ����ʹ�ã��൱�ڴ���
        cout<<"�� Listen Socket ����ɶ˿�ʧ��! ������룺"<<WSAGetLastError()<<endl;
        RELEASE_SOCKET(m_pListenContext->m_Socket);
        return false;
    }
    else
        cout<<"Listen Socket ����ɶ˿ڳɹ�!"<<endl;
    this->GetLocalIP();
    //��ʼ����������ַ��Ϣ
    ZeroMemory((sockaddr*)&ServerAddress,sizeof(ServerAddress));
    ServerAddress.sin_family=AF_INET;
    ServerAddress.sin_addr.s_addr = inet_addr(m_strIP.c_str());
    ServerAddress.sin_port=htons(m_Port);

    //�󶨵�ַ�Ͷ˿�
    if(SOCKET_ERROR==bind(m_pListenContext->m_Socket,(sockaddr*)&ServerAddress,sizeof(ServerAddress)))
    {
        cout<<"bind()����ִ��ʧ��!"<<endl;
        return false;
    }
    else
        cout<<"bind()����ִ�гɹ�!"<<endl;

    //��ʼ���м���
    if(SOCKET_ERROR==listen(m_pListenContext->m_Socket,SOMAXCONN))
    {
        cout<<"listen()����ִ��ʧ��!"<<endl;
        return false;
    }

    //����ʹ��AcceptEx()����  ��  GetAcceptExSockAddrs������������Ҫ���ָ��
    GUID GuidAcceptEx = WSAID_ACCEPTEX;
    GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;   //�������������ڵ�������ָ��
    DWORD dwBytes=0;
    if(SOCKET_ERROR==WSAIoctl(m_pListenContext->m_Socket,SIO_GET_EXTENSION_FUNCTION_POINTER,&GuidAcceptEx,sizeof(GuidAcceptEx),
                              &m_lpfnAcceptEx,sizeof(m_lpfnAcceptEx),&dwBytes,NULL,NULL))

    {
        cout<<"δ�ܻ��AcceptEx()����ָ��! ������룺"<<WSAGetLastError()<<endl;
        DeInitializeSore();
        return false;
    }

    if(SOCKET_ERROR==WSAIoctl(m_pListenContext->m_Socket,SIO_GET_EXTENSION_FUNCTION_POINTER,&GuidGetAcceptExSockAddrs,sizeof(GuidGetAcceptExSockAddrs),
                              &m_lpfnGetAcceptExSockAddrs,sizeof(m_lpfnGetAcceptExSockAddrs),&dwBytes,NULL,NULL))

    {
        cout<<"δ�ܻ��GetAcceptExSockAddrs����ָ��! ������룺"<<WSAGetLastError()<<endl;
        DeInitializeSore();
        return false;
    }

    //��ú���ָ��֮�󣬿�ʼΪAcceptEx����׼��������Ͷ��AcceptEx IO����
    for(int i=0;i<200;++i)
    {
        PER_IO_CONTEXT* pAcceptIoContext = m_pListenContext->GetNewIoContext(); //GetNewIoContext()����ֻ�Ƿ���һ���ռ� �Ž�vector���� ��û�д�����Ϣ
        if(false==this->PostAccept(pAcceptIoContext))
        {
           m_pListenContext->RemoveContext(pAcceptIoContext);
           return false;
        }
    }
    cout<<"Ͷ�� 30 ��AcceptEx�������!"<<endl;
    return true;
}




//����ͷ�������Դ
void IOCPMODEL::DeInitializeSore()
{
    //ɾ���ͻ��˻������
    DeleteCriticalSection(&m_csContextList);

    //�ر�ϵͳ�Ƴ��¼����
    RELEASE_HANDLE(m_hShutdownEvent);

    //�ͷŹ������߳̾��
    for(int i=0;i<m_nThread;++i)
    {
        RELEASE_HANDLE(m_hWorkerThread[i]);
    }
    RELEASE(m_hWorkerThread); //�ͷŵ��������߳�ָ��

    RELEASE_HANDLE(m_hIOCompletionPort);//�ͷŵ���ɶ˿ھ��

    RELEASE(m_pListenContext); //�ͷŵ������׽���ָ��

    cout<<"��Դ�ͷ����!"<<endl;
}

//Ͷ��AcceptEx����
bool IOCPMODEL::PostAccept(PER_IO_CONTEXT *pAcceptIoContext)
{
    assert(INVALID_SOCKET!=m_pListenContext->m_Socket);

    //׼������
    DWORD dwBytes=0;
    pAcceptIoContext->m_OpType=ACCEPT_POSTED;
    WSABUF *P_wbuf = &pAcceptIoContext->m_wsaBuf;
    OVERLAPPED *p_ol=&pAcceptIoContext->m_Overlapped;

    //Ϊ�Ժ�������Ŀͻ�����׼����Socket(�����봫ͳ��Accept��������)
    pAcceptIoContext->m_sockAccept=WSASocket(AF_INET,SOCK_STREAM,IPPROTO_TCP,NULL,0,WSA_FLAG_OVERLAPPED);
    if(INVALID_SOCKET == pAcceptIoContext->m_sockAccept)
    {
        cout<<"��ǰ��������AcceptEx()��Socketʧ��! ������룺"<<WSAGetLastError()<<endl;
        return false;
    }

    //Ͷ��AcceptEx�����øú���
    if(false==m_lpfnAcceptEx( m_pListenContext->m_Socket,pAcceptIoContext->m_sockAccept,P_wbuf->buf,P_wbuf->len-((sizeof(SOCKADDR_IN)+16)*2),
                              sizeof(SOCKADDR_IN)+16,sizeof(SOCKADDR_IN)+16,&dwBytes,p_ol))
    {
        if(WSA_IO_PENDING != WSAGetLastError())
        {
           cout<<"Ͷ��AcceptEx����ʧ��! �������룺"<<WSAGetLastError()<<endl;
           return false;
        }
    }
    return true;
}

//// Ͷ�ݽ�����������
bool IOCPMODEL::PostRecv(PER_IO_CONTEXT *pIoContext)
{
    // ��ʼ������
        DWORD dwFlags = 0;                                //�������������Ҫ׼���ô�����ݵĻ������ͺ���
        DWORD dwBytes = 0;
        WSABUF *p_wbuf   = &pIoContext->m_wsaBuf;
        OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

        pIoContext->ResetBuffer();
        pIoContext->m_OpType = RECV_POSTED;

        // ��ʼ����ɺ󣬣�Ͷ��WSARecv����
        int nBytesRecv = WSARecv( pIoContext->m_sockAccept, p_wbuf, 1, &dwBytes, &dwFlags, p_ol, NULL );

        // �������ֵ���󣬲��Ҵ���Ĵ��벢����Pending�Ļ����Ǿ�˵������ص�����ʧ����
        if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
        {
            cout<<"Ͷ�ݵ�һ��WSARecvʧ�ܣ�"<<endl;
            return false;
        }
        return true;
}


//���пͻ�������ʱ  ���д���   �������ListenSocket��Context��������Ҫ����һ�ݳ������������Socket��   ԭ����Context����Ҫ���������Ͷ����һ��Accept����
bool IOCPMODEL::DoAccept(PER_SOCKET_CONTEXT *pSocketContext, PER_IO_CONTEXT *pIoContext)
{
    //���Ȼ�ÿͻ��˵ĵ�ַ��Ϣ   ����GetAcceptExSockAddrs����   ����ȡ�ÿͻ��˵�ַ��Ϣ   ���ܻ�ÿͻ��˷����ĵ�һ������
    SOCKADDR_IN* ClientAddr = NULL;
    SOCKADDR_IN* LocalAddr = NULL;
    int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);

    this->m_lpfnGetAcceptExSockAddrs(pIoContext->m_wsaBuf.buf,pIoContext->m_wsaBuf.len-((sizeof(SOCKADDR_IN)+16)*2),
                                     sizeof(SOCKADDR_IN)+16, sizeof(SOCKADDR_IN)+16,(LPSOCKADDR*)&LocalAddr,&localLen,
                                     (LPSOCKADDR*)&ClientAddr,&remoteLen);
   // cout<<"�ͻ��� "<<inet_ntoa(ClientAddr->sin_addr)<<": "<<ntohs(ClientAddr->sin_port)<<" ����!"<<endl;
  //  cout<<"�ͻ��� "<<inet_ntoa(ClientAddr->sin_addr)<<": "<<ntohs(ClientAddr->sin_port)<<" ����Ϣ�ǣ�"<<pIoContext->m_wsaBuf.buf<<endl;



    //������Ҫע�⣬���ﴫ��������ListenSocket�ϵ�Context�����Context���ǻ���Ҫ���ڼ�����һ������
    // �����һ���Ҫ��ListenSocket�ϵ�Context���Ƴ���һ��Ϊ�������Socket�½�һ��SocketContext
    PER_SOCKET_CONTEXT* pNewSocketContext = new PER_SOCKET_CONTEXT;
    pNewSocketContext->m_Socket           = pIoContext->m_sockAccept;
    memcpy(&(pNewSocketContext->m_ClientAddr), ClientAddr, sizeof(SOCKADDR_IN));
    // ����������ϣ������Socket����ɶ˿ڰ�(��Ҳ��һ���ؼ�����)
        if( false==this->AssociateWithIOCP( pNewSocketContext ) )
        {
            RELEASE( pNewSocketContext );
            return false;
        }

        // 3. �������������µ�IoContext�����������Socket��Ͷ�ݵ�һ��Recv��������
        PER_IO_CONTEXT* pNewIoContext = pNewSocketContext->GetNewIoContext();
        pNewIoContext->m_OpType       = RECV_POSTED;
        pNewIoContext->m_sockAccept   = pNewSocketContext->m_Socket;
        // ���Buffer��Ҫ���������Լ�����һ�ݳ���
        //memcpy( pNewIoContext->m_szBuffer,pIoContext->m_szBuffer,MAX_BUFFER_LEN );

        // �����֮�󣬾Ϳ��Կ�ʼ�����Socket��Ͷ�����������
        if( false==this->PostRecv( pNewIoContext) )
        {
            pNewSocketContext->RemoveContext( pNewIoContext );
            return false;
        }

        // 4. ���Ͷ�ݳɹ�����ô�Ͱ������Ч�Ŀͻ�����Ϣ�����뵽ContextList��ȥ(��Ҫͳһ���������ͷ���Դ)
        this->AddToContextList( pNewSocketContext );


        // 5. ʹ�����֮�󣬰�Listen Socket���Ǹ�IoContext���ã�Ȼ��׼��Ͷ���µ�AcceptEx
        pIoContext->ResetBuffer();
        return this->PostAccept( pIoContext );


}

//���н��յ����ݵ����ʱ�򣬽��д���
bool IOCPMODEL::DoRecv(PER_SOCKET_CONTEXT *pSocketContext, PER_IO_CONTEXT *pIoContext)
{

    // �Ȱ���һ�ε�������ʾ���֣�Ȼ�������״̬��������һ��Recv����
        SOCKADDR_IN* ClientAddr = &pSocketContext->m_ClientAddr;
       // cout<<"�յ�"<<inet_ntoa(ClientAddr->sin_addr)<<": "<<ntohs(ClientAddr->sin_port)<< "��Ϣ��"<<pIoContext->m_wsaBuf.buf<<endl;

        // Ȼ��ʼͶ����һ��WSARecv����
        return PostRecv( pIoContext );
}

//���ͻ��˵������Ϣ�洢��������
void IOCPMODEL::AddToContextList(PER_SOCKET_CONTEXT *pHandleData)
{
    EnterCriticalSection(&m_csContextList);

    m_arrayClientContext.push_back(pHandleData);

    LeaveCriticalSection(&m_csContextList);
}


//�Ƴ�ĳ���ض���Context
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


//��տͻ�����Ϣ
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



//�����(Socket)�󶨵���ɶ˿���
bool IOCPMODEL::AssociateWithIOCP(PER_SOCKET_CONTEXT *pContext)
{
    // �����ںͿͻ���ͨ�ŵ�SOCKET�󶨵���ɶ˿���
        HANDLE hTemp = CreateIoCompletionPort((HANDLE)pContext->m_Socket, m_hIOCompletionPort, (DWORD)pContext, 0);

        if (NULL == hTemp)
        {
            cout<<"ִ��CreateIoCompletionPort()���ִ���.������룺"<<GetLastError()<<endl;
            return false;
        }

        return true;
}

//��ʾ��������ɶ˿��ϵĴ���
bool IOCPMODEL::HandleError(PER_SOCKET_CONTEXT *pContext, const DWORD &dwErr)
{
    // ����ǳ�ʱ�ˣ����ټ����Ȱ�
        if(WAIT_TIMEOUT == dwErr)
        {
            // ȷ�Ͽͻ����Ƿ񻹻���...
            if( !IsSocketAlive( pContext->m_Socket) )
            {
                cout<<"��⵽�ͻ����쳣�˳�!"<<endl;;
                this->_RemoveContext( pContext );
                return true;
            }
            else
            {
                cout<<"���������ʱ! ������..."<<endl;
                return true;
            }
        }

        // �����ǿͻ����쳣�˳���
        else if( ERROR_NETNAME_DELETED==dwErr )
        {
            cout<<"��⵽�ͻ����쳣�˳���"<<endl;
            this->_RemoveContext( pContext );
            return true;
        }

        else
        {
            cout<<"��ɶ˿ڲ������ִ����߳��˳���������룺"<<dwErr<<endl;
            return false;
        }
}



//����������
bool IOCPMODEL::Start()
{
     //����һ��ϵͳ�̻߳������
     InitializeCriticalSection(&m_csContextList);

     //����ϵͳ�˳����¼�֪ͨ
     m_hShutdownEvent=CreateEvent(NULL,TRUE,FALSE,NULL);

     //��ʼ��IOCP
     if(false==InitializeIOCP())
     {
         cout<<"IOCP��ʼ��ʧ��!"<<endl;
         return false;
     }
     else
         cout<<"IOCP��ʼ�����!!"<<endl;

     //��ʼ��Socket
     if(false==InitializeSocket())
     {
         cout<<"Listen Socket��ʼ��ʧ��!"<<endl;
         return false;
     }
     else
         cout<<"Listen Socket��ʼ�����!!"<<endl;

     cout<<"ϵͳ׼���������Ⱥ�����!"<<endl;
     return true;
}

//��ʼ����ϵͳ�˳���Ϣ���˳���ɶ˿ں��߳���Դ
void IOCPMODEL::Stop()
{
    if( m_pListenContext!=NULL && m_pListenContext->m_Socket!=INVALID_SOCKET )
        {
            // ����ر���Ϣ֪ͨ
            SetEvent(m_hShutdownEvent);

            for (int i = 0; i < m_nThread; i++)
            {
                // ֪ͨ���е���ɶ˿ڲ����˳�
                PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD)EXIT_CODE, NULL);
            }

            // �ȴ����еĿͻ�����Դ�˳�
            WaitForMultipleObjects(m_nThread, m_hWorkerThread, TRUE, INFINITE);

            // ����ͻ����б���Ϣ
            this->ClearContextList();

            // �ͷ�������Դ
            this-> DeInitializeSore();

            cout<<"ֹͣ����"<<endl;
        }
}


//����winsock2
bool IOCPMODEL::LoadSocket()
{
    WSADATA wsadata;
    int result=WSAStartup(MAKEWORD(2,2),&wsadata);
    if(NO_ERROR!=result)
    {
        cout<<"��ʼ��Winsock2.2ʧ��!"<<endl;
        return false;
    }
    else
        cout<<"��ʼ��Winsock2.2�ɹ�!"<<endl;
    return true;
}


//��ñ�����IP��ַ
string IOCPMODEL::GetLocalIP()
{
    // ��ñ���������
        char hostname[MAX_PATH] = {0};
        gethostname(hostname,MAX_PATH);
        struct hostent FAR* lpHostEnt = gethostbyname(hostname);
        if(lpHostEnt == NULL)
        {
            return DEFAULT_IP;
        }

        // ȡ��IP��ַ�б��еĵ�һ��Ϊ���ص�IP(��Ϊһ̨�������ܻ�󶨶��IP)
        LPSTR lpAddr = lpHostEnt->h_addr_list[0];

        // ��IP��ַת�����ַ�����ʽ
        struct in_addr inAddr;
        memmove(&inAddr,lpAddr,4);
        m_strIP = string( inet_ntoa(inAddr) );

        return m_strIP;
}

//��ÿͻ���socket����
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
    cout<<"�ͻ��� "<<inet_ntoa(temp->m_ClientAddr.sin_addr)<<": "<<ntohs(temp->m_ClientAddr.sin_port)<<" �Ͽ�����."<<endl;
    LeaveCriticalSection(&m_csContextList);
}


//��ʼ����ɶ˿�
bool IOCPMODEL::InitializeIOCP()
{
    //������һ����ɶ˿�
    m_hIOCompletionPort=CreateIoCompletionPort(INVALID_HANDLE_VALUE,NULL,0,0);
    if(NULL==m_hIOCompletionPort)
    {
        cout<<"������ɶ˿�ʧ��!�������Ϊ��"<<WSAGetLastError()<<endl;
        return false;
    }
    else
        cout<<"������ɶ˿ڳɹ�!"<<endl;

    //���ݴ��������������߳�
    m_nThread=GetNumOfProcessers()*2;
    m_hWorkerThread =new HANDLE[m_nThread];  //�����߳�����  �������ͷ�
    DWORD nThreadID;//�߳�id
    for(int i=0;i<m_nThread;++i)
    {
        //�Լ�������̲߳���
        THREADPARAMS_WORKER* pThreadParams=new THREADPARAMS_WORKER;
        pThreadParams->pIOCPModel=this;
        pThreadParams->nThreadNo=i+1;
        m_hWorkerThread[i]=CreateThread(0,0,WorkerThread,(void*)pThreadParams,0,&nThreadID);
    }

    cout<<"�����������߳� "<<m_nThread<<" ��! ������ȫ����!"<<endl;
    return TRUE;
}


//���ϵͳ����������
int IOCPMODEL::GetNumOfProcessers()
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
}

//�жϿͻ���Socket�Ƿ��Ѿ��Ͽ���������һ����Ч��Socket��Ͷ��WSARecv����������쳣
bool IOCPMODEL::IsSocketAlive(SOCKET s)
{
    int nByteSent=send(s,"",0,0);
    if (-1 == nByteSent) return false;
    return true;
}












