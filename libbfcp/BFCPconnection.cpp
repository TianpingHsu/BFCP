#include "BFCPconnection.h"

#include <iostream>
#include <map>
#include <sstream>

#include "BFCPexception.h"

using ::std::map;
using ::std::pair;

#ifdef WIN32

#include <io.h>

static std::string GetErrorText() {
    DWORD lastError = GetLastError();
    LPVOID lpMsgBuf;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL, lastError, 0, (LPTSTR)&lpMsgBuf, 0, NULL);
    std::string errstr((const char *)lpMsgBuf);
    LocalFree(lpMsgBuf);
    return errstr;
}

static int gettimeofday(struct timeval *tv, struct timezone *) {
    FILETIME ft;
    unsigned __int64 tmpres = 0;
    static int tzflag;

    GetSystemTimeAsFileTime(&ft);

    tmpres |= ft.dwHighDateTime;
    tmpres <<= 32;
    tmpres |= ft.dwLowDateTime;

    /*converting file time to unix epoch*/
    tmpres -= 11644473600000000ULL;
    tmpres /= 10; /*convert into microseconds*/
    tv->tv_sec = (long)(tmpres / 1000000UL);
    tv->tv_usec = (long)(tmpres % 1000000UL);

    return 0;
}

#else  // WIN32

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <vector>

std::string GetErrorText() {
    std::vector<char> buf;
    buf.resize(1000);
#if defined(__APPLE__) || ((_POSIX_C_SOURCE >= 200112L) && !_GNU_SOURCE)
    int err = strerror_r(errno, buf.data(), buf.size() - 1);
    return err != 0 ? "?" : buf.data();
#else
    return strerror_r(errno, buf.data(), buf.size() - 1);
#endif
}

#endif  // WIN32

static inline const char *TRANSPORT_NAME(int p_transp) {
    switch (p_transp) {
        case BFCP_OVER_TCP:
            return "TCP";

        case BFCP_OVER_TLS:
            return "TLS";

        case BFCP_OVER_UDP:
            return "UDP";

        default:
            return "unknown";
    }
}

static inline bool IsTransactionStart(e_bfcp_primitives mtype) {
    return (mtype == e_primitive_FloorRequest ||
            mtype == e_primitive_FloorRelease ||
            mtype == e_primitive_FloorRequestStatus ||
            mtype == e_primitive_FloorRequestQuery ||
            mtype == e_primitive_UserQuery || mtype == e_primitive_FloorQuery ||
            mtype == e_primitive_ChairAction || mtype == e_primitive_Hello ||
            mtype == e_primitive_Goodbye);
}

static inline bool IsTransactionAnswer(e_bfcp_primitives mtype) {
    return (
        mtype == e_primitive_FloorRequestStatus ||
        mtype == e_primitive_UserStatus || mtype == e_primitive_FloorStatus ||
        mtype == e_primitive_ChairActionAck || mtype == e_primitive_HelloAck ||
        mtype == e_primitive_FloorRequestStatusAck ||
        mtype == e_primitive_FloorStatusAck || mtype == e_primitive_GoodbyeAck);
}

const int BFCPConnectionRole::ACTIVE = 0;
const int BFCPConnectionRole::PASSIVE = 1;

/*********************************
 * isZeroTime
 *	Check if time val is 0
 *********************************/
#if 0  // Unused
static inline bool isZeroTime(const struct timeval *val)
{
        //Check if it is zero
        return !(val->tv_sec & val->tv_usec);
}
#endif

static int compareTime(const struct timeval *t1, const struct timeval *t2) {
    if (t1->tv_sec > t2->tv_sec) return 2;

    if (t1->tv_sec < t2->tv_sec) return -2;

    // Here: t1->tv_sec == t2->tv_sec

    if (t1->tv_usec > t2->tv_usec) return 1;

    if (t1->tv_usec < t2->tv_usec) return -1;

    return 0;
}

BFCPConnectionRole::BFCPConnectionRole(void) {}

BFCPConnectionRole::~BFCPConnectionRole(void) {}

BFCPConnection::BFCPConnection(int transport) : m_remoteClient(transport) {
    bfcp_mutex_init(m_mutConnect, NULL);
    bfcp_mutex_init(m_SessionMutex, NULL);
    m_ClientSocket.clear();
    m_Socket = BFCP_INVALID_SOCKET;
    m_bClose = false;
    m_eRole = BFCPConnectionRole::ACTIVE;
    m_bConnected = false;
    m_isStarted = false;

#ifdef WIN32
    WSADATA wsaData;
    int err = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (err < 0)
        throw BFCPException("BFCPConnection", __LINE__, "Winsock",
                            "BFCP start TCP connect  WSAStartup failed !");
#else
    pthread_cond_init(&m_timer_cond, 0);
    FD_ZERO(&m_wset);
    if (pipe(pipefd) != 0)
        throw BFCPException("BFCPConnection", __LINE__, "Internal pipe",
                            "Failed to open internal pipe");
#endif
    m_thread = BFCP_NULL_THREAD_HANDLE;
    m_timer_thread = BFCP_NULL_THREAD_HANDLE;
    //Log(INF, "%s %p\n", __FUNCTION__, this);
}

BFCPConnection::~BFCPConnection(void) {
    //Log(INF, "%s %p\n", __FUNCTION__, this);
    disconnect();

    bfcp_mutex_lock(m_mutConnect);
    bfcp_mutex_destroy(m_mutConnect);
    bfcp_mutex_lock(m_SessionMutex);
    bfcp_mutex_destroy(m_SessionMutex);

#ifdef WIN32
    WSACleanup();
#else
    pthread_cond_destroy(&m_timer_cond);
    close(pipefd[0]);
    close(pipefd[1]);
#endif
}

bool BFCPConnection::SelLocalConnection(const char *localAddress, UINT16 port,
                                        int eRole) {
    if (m_bConnected) return false;

    m_eRole = eRole;
    m_remoteClient.SetRole(eRole);
    return m_remoteClient.SetLocalAddress(localAddress, port);
}

const char *BFCPConnection::getLocalAdress() {
    return m_remoteClient.GetLocalAddr();
}

UINT16 BFCPConnection::getLocalPort() { return m_remoteClient.GetLocalPort(); }

bool BFCPConnection::setRemoteEndpoint(const char *address, UINT16 port) {
    if (!address || !port || m_bConnected) return false;

    m_remoteClient.SetRemoteAddress(address, port);
    return true;
}

const char *BFCPConnection::getRemoteAdress() {
    return m_remoteClient.GetRemoteAddr();
}

UINT16 BFCPConnection::getRemotePort() {
    return m_remoteClient.GetRemotePort();
}

void BFCPConnection::addSession(const std::string &sessionId) {
    bfcp_mutex_lock(m_SessionMutex);

    // m_mapSessions[sessionId] = stream;
    bfcp_mutex_unlock(m_SessionMutex);
}

void BFCPConnection::removeSession() {
    if (m_bConnected || !m_bClose || m_isStarted) {
        // bfcp_mutex_lock(m_SessionMutex);
        // bfcp_mutex_unlock(m_SessionMutex);
    }

    disconnect();
}

bool BFCPConnection::IsClientActive(BFCP_SOCKET s) {
    bool lock = false;
    bool ret = true;
    std::map<BFCP_SOCKET, Client2ServerInfo>::iterator it;

    if (s == BFCP_INVALID_SOCKET) return false;

    if (BFCP_CURRENT_THREAD() != m_thread) {
        bfcp_mutex_lock(m_mutConnect);
        lock = true;
    }

    it = m_ClientSocket.find(s);
    if (it == m_ClientSocket.end()) {
        /* not a client socket */
        if (lock) bfcp_mutex_unlock(m_mutConnect);
        return false;
    }

    ret = (it->second.GetRemotePort() != 0);
    if (lock) bfcp_mutex_unlock(m_mutConnect);
    return ret;
}

bool BFCPConnection::SetRemoteAddressAndPort(BFCP_SOCKET s,
                                             const char *remoteIp,
                                             UINT16 remotePort) {
    bool lock = false;
    std::map<BFCP_SOCKET, Client2ServerInfo>::iterator it;

    if (s == BFCP_INVALID_SOCKET) return false;

    if (BFCP_CURRENT_THREAD() != m_thread) {
        bfcp_mutex_lock(m_mutConnect);
        lock = true;
    }

    it = m_ClientSocket.find(s);
    if (it == m_ClientSocket.end()) {
        /* not a client socket */
        if (lock) bfcp_mutex_unlock(m_mutConnect);
        return false;
    }

    it->second.SetRemoteAddress(remoteIp, remotePort);

    if (lock) bfcp_mutex_unlock(m_mutConnect);
    return true;
}

/* Send an already composed message (buffer) to the FCS */
int BFCPConnection::sendBFCPmessage(BFCP_SOCKET s, bfcp_message *message,
                                    bool retrans) {
    int ret, transp;

    if (s == BFCP_INVALID_SOCKET) return -1;
    if (message == NULL) return -1;
    if (m_bClose) return -1;

    if (s == m_Socket) {
        transp = m_remoteClient.GetTransport();
        if ((transp == BFCP_OVER_TCP || transp == BFCP_OVER_TLS) &&
            !m_bConnected) {
            /* active socket that is not connected. We cannot send data on it*/
            Log(ERR,
                "FD [%d] is using TCP or TLS transport and is not connected.",
                s);
            return -3;
        }
        ret = m_remoteClient.SendData(this, s, message);
    } else {
        std::map<BFCP_SOCKET, Client2ServerInfo>::iterator it;
        bool lock = false;

        if (BFCP_CURRENT_THREAD() != m_thread) {
            bfcp_mutex_lock(m_mutConnect);
            lock = true;
        }

        it = m_ClientSocket.find(s);
        if (it == m_ClientSocket.end()) {
            /* not a client socket */
            if (lock) bfcp_mutex_unlock(m_mutConnect);
            Log(ERR, "Invalid FD [%d] - no in the client list", s);
            return -5;
        }

        transp = it->second.GetTransport();
        ret = it->second.SendData(this, s, message);
        if (lock) {
            bfcp_mutex_unlock(m_mutConnect);
        }
    }

    if (ret < 0) return ret;

    if (transp == BFCP_OVER_UDP && !retrans) {
        /*
         * This was not a retransmission and transport is not reliable
         * we have to store transaction in a table in order to manage
         * retransmission
         */
        UINT16 transID = bfcp_get_transactionID(message);
        if (transID != 0) {
            Transaction t(s, message);

            bfcp_mutex_lock(m_SessionMutex);
            if (IsTransactionStart(bfcp_get_primitive(message)))
                transactionMap[transID] = t;
            bfcp_mutex_unlock(m_SessionMutex);
#ifndef WIN32
            pthread_cond_signal(&m_timer_cond);
#endif
        } else {
            Log(ERR,
                "BFCP message is starting transaction and transaction ID is 0");
        }
    }
    return 0;
}

int BFCPConnection::CloseOutgoingTransaction(BFCP_SOCKET s, bfcp_message *m) {
    if (IsTransactionAnswer(bfcp_get_primitive(m))) {
        /*
         * This was not a retransmission and transport is not reliable
         * we have to store transaction in a table in order to manage
         * retransmission
         */

        UINT16 transID = bfcp_get_transactionID(m);

        if (transID != 0) {
            bfcp_mutex_lock(m_SessionMutex);
            transactionMap.erase(transID);
            bfcp_mutex_unlock(m_SessionMutex);
#ifndef WIN32
            if (m_timer_thread != BFCP_NULL_THREAD_HANDLE)
                pthread_cond_signal(&m_timer_cond);
#endif
            return 1;
        }
        return -1;
    }
    return 0;
}

bool BFCPConnection::Client2ServerInfo::HandleRemoteRetrans(BFCPConnection *c,
                                                            BFCP_SOCKET s,
                                                            bfcp_message *m) {
#if 0  // Causes problems, needs investigation
    if ( IsTransactionStart(bfcp_get_primitive(m)) )
    {
	/*
	 * This message initiates a transaction. We need to check if there is an existing
	 * transaction in the answerMap and resend the answer if needed !
	 */
	UINT16 transID = bfcp_get_transactionID(m);
	std::map<UINT16, Transaction>::iterator it;
	if (transID != 0)
	{
	    it = answerMap.find(transID);
	    if (it != answerMap.end())
	    {
		c->Log(INF, "Detected retransmission tr ID %u. Resending the same answer", transID);
		SendData( c, s, it->second.message );
		return true;
	    }
	}
    }
#endif
    return false;
}

int BFCPConnection::Client2ServerInfo::CheckExpiredAnswers(BFCPConnection *c) {
    std::map<UINT16, Transaction>::iterator it;
    struct timeval waituntil;
    UINT16 trID = 0;
    ;

    for (it = answerMap.begin(); it != answerMap.end(); it++) {
        if (it->second.CheckTimerT1(&waituntil, NULL) < 0) {
            /* Expired T1 */
            trID = it->first;
            if (it->second.message != NULL &&
                bfcp_get_primitive(it->second.message) ==
                    e_primitive_GoodbyeAck) {
                /*
                 * Special case where we answered to a remote goodbye message.
                 * The server did not close the connection and remove the client
                 * in order to be able to handle Goodbye retransmission from
                 * remote party but now T1 has expired so we indicate to the
                 * application that the connection is really gone
                 */
                return -1;
            }
            break;
        }
    }

    if (trID > 0) {
        answerMap.erase(trID);
        return 1;
    }
    return 0;
}

void BFCPConnection::Transaction::MarkTransmission() {
    if (timerDuration == 0) {
        timerDuration = 500;
        gettimeofday(&timerExpiration, 0);
        timerExpiration.tv_usec += 500 * 1000;
    } else if (timerDuration <= 16000) {
        timerDuration = timerDuration * 2;

        gettimeofday(&timerExpiration, 0);
        timerExpiration.tv_sec += timerDuration / 1000;
        timerExpiration.tv_usec += (timerDuration % 1000) * 1000;
    }
}

int BFCPConnection::Transaction::CheckTimerT1(struct timeval *waituntil,
                                              unsigned int *duration) {
    struct timeval now;
    int ret;

    /* transaction has expired */
    if (timerDuration == 0) {
        MarkTransmission();
        return 0;
    }

    if (timerDuration > 16000) return -1;

    gettimeofday(&now, 0);
    if (compareTime(&now, &timerExpiration) >= 0) {
        /* timer has expired */
        if (duration) *duration = timerDuration;
        ret = 1;
    } else {
        ret = 0;
    }

    if (waituntil != NULL && compareTime(waituntil, &timerExpiration) >= 0) {
        *waituntil = timerExpiration;
    }

    return ret;
}

int BFCPConnection::connect() {
    bfcp_mutex_lock(m_mutConnect);
    Log(INF, "BFCP start TCP connect role[%s]",
        m_eRole == BFCPConnectionRole::PASSIVE ? "server" : "client");
    if (m_isStarted) {
        Log(INF, "BFCP start transport connect , aldready connected !");
    } else {
        m_bClose = false;
        m_isStarted = true;
        BFCP_THREAD_START(m_thread, BFCPConnection::EntryPoint, this);
        BFCP_THREAD_START(m_timer_thread, BFCPConnection::ManageRetransmission, this);
        int count = 2000;
        int waitRange = 2;
        while (count > 0 && !m_bConnected && m_isStarted) {
            BFCP_SLEEP(waitRange);
            count -= waitRange;
        }
        if (m_bConnected)
            Log(INF, "BFCP transport thread role[%s] started",
                m_eRole == BFCPConnectionRole::PASSIVE ? "server" : "client");
        else {
            Log(ERR, "BFCP TCP connect role[%s] failed ",
                m_eRole == BFCPConnectionRole::PASSIVE ? "server" : "client");
            BFCP_THREAD_KILL(m_thread);
            m_isStarted = false;
            m_bConnected = false;
        }
    }
    bfcp_mutex_unlock(m_mutConnect);
    return m_bConnected;
}

/*-----------------------------------------------------------------------------------------*/
void BFCPConnection::disconnect() {
    m_bClose = true;
    if (m_bConnected || m_isStarted) {
        try {
            Log(INF,
                "BFCP stop TCP disconnect role[%s] nbclient[%d] connected[%s] "
                "started[%s] close request[%s]",
                m_eRole == BFCPConnectionRole::PASSIVE ? "server" : "client",
                m_ClientSocket.size(), m_bConnected ? "true" : "false",
                m_isStarted ? "true" : "false", m_bClose ? "true" : "false");

            bfcp_mutex_lock(m_mutConnect);
            if (!m_ClientSocket.empty()) {
                std::map<BFCP_SOCKET, Client2ServerInfo>::iterator it;
                for (it = m_ClientSocket.begin(); it != m_ClientSocket.end();
                     it++) {
                    BFCP_SOCKET s = it->first;
                    it->second.CloseSocket(s);
                }
                m_ClientSocket.clear();
            }
            bfcp_mutex_unlock(m_mutConnect);
        } catch (...) {
            bfcp_mutex_unlock(m_mutConnect);
            Log(ERR, "Exception catched in thread");
        }

        try {
            int count = 2000;
            int waitRange = 2;

            m_remoteClient.CloseSocket(m_Socket);
            m_Socket = BFCP_INVALID_SOCKET;

            while (count > 0 && (m_bConnected || m_isStarted)) {
                BFCP_SLEEP(waitRange);
                count -= waitRange;
            }

            // Wait for server thread to close
            Log(INF, "BFCP TCP disconnect role[%s] wait end of server [0x%p]",
                m_eRole == BFCPConnectionRole::PASSIVE ? "server" : "client",
                m_thread);
#ifndef WIN32
            if (m_timer_thread) {
                pthread_cond_signal(&m_timer_cond);
                pthread_join(m_timer_thread, NULL);
            }
#endif

            if (m_thread) {
#ifndef WIN32
                pthread_join(m_thread, NULL);

                Log(INF, "BFCP TCP disconnect role[%s] success",
                    m_eRole == BFCPConnectionRole::PASSIVE ? "server"
                                                           : "client");
#else
                if (m_bConnected || m_isStarted) {
                    Log(ERR, "BFCP TCP disconnect role[%s] failed ",
                        m_eRole == BFCPConnectionRole::PASSIVE ? "server"
                                                               : "client");
                    BFCP_THREAD_KILL(m_thread);
                    BFCP_THREAD_KILL(m_timer_thread);
                    m_isStarted = false;
                    m_bConnected = false;
                    m_thread = BFCP_NULL_THREAD_HANDLE;
                } else {
                    Log(INF, "BFCP TCP disconnect role[%s] success",
                        m_eRole == BFCPConnectionRole::PASSIVE ? "server"
                                                               : "client");
                }
#endif
            }
        } catch (...) {
            Log(ERR, "Exception catched in thread");
        }
    }
}

#ifdef WIN32
unsigned __stdcall BFCPConnection::ManageRetransmission(void *pParam)
#else
void *BFCPConnection::ManageRetransmission(void *pParam)
#endif
{
    BFCPConnection *c = (BFCPConnection *)pParam;
    struct timeval waituntil;
    UINT16 tID = 0;
    std::map<UINT16, Transaction>::iterator it;
    std::map<UINT16, Transaction> retTransactions;

    int ret;

    c->Log(INF, ">> BFCPConnection: retransmission thread starting. %p", c);

    bfcp_mutex_lock(c->m_SessionMutex);
    while (!c->m_bClose) {
        tID = 0;
        bool need_disconnect = false;

        /* By default, wait 30 seconds*/
        gettimeofday(&waituntil, NULL);
        waituntil.tv_sec += 30;
        unsigned int duration;

        retTransactions.clear();

        for (it = c->transactionMap.begin(); it != c->transactionMap.end();
             it++) {
            ret = it->second.CheckTimerT1(&waituntil, &duration);
            tID = it->first;

            switch (ret) {
                case 1:
                    c->Log(INF,
                           "-BFCPConnection: resending message for transaction "
                           "%u after %u ms",
                           tID, duration);
                    retTransactions[tID] = it->second;

                    // ret = c->sendBFCPmessage(it->second.m_sockfd,
                    // it->second.message, true);
                    it->second.MarkTransmission();
                    break;

                default:
                case 0:
                    break;

                case -1:
                    /* If a request is not answered we signal a disconnction as
                     * per the BFCP over UDP RFC */
                    c->Log(INF,
                           "-BFCPConnection: outgoing transaction %u has "
                           "expired. Socket %d will be closed",
                           tID, it->second.m_sockfd);
                    need_disconnect = true;
                    break;
            }
            if (need_disconnect) break;
        }

        bfcp_mutex_unlock(c->m_SessionMutex);

        if (need_disconnect) {
            c->OnBFCPDisconnected(it->second.m_sockfd);
            gettimeofday(&waituntil, NULL);
            bfcp_mutex_lock(c->m_SessionMutex);
            c->transactionMap.erase(tID);
            bfcp_mutex_unlock(c->m_SessionMutex);

            /* Map has been modified, we exit the loop otherwise, it will crash
             */
            break;
        }

        /* Do the retransmission ! */
        for (it = retTransactions.begin(); it != retTransactions.end(); it++) {
            ret = c->sendBFCPmessage(it->second.m_sockfd, it->second.message,
                                     true);
            if (ret == -3) {
                c->OnBFCPDisconnected(it->second.m_sockfd);
                gettimeofday(&waituntil, NULL);
                bfcp_mutex_lock(c->m_SessionMutex);
                c->transactionMap.erase(tID);
                bfcp_mutex_unlock(c->m_SessionMutex);
                /* Map has been modified, we exit the loop otherwise, it will
                 * crash */
            }
        }
        bfcp_mutex_lock(c->m_SessionMutex);

#ifndef WIN32
        /* now wait for the next retransmission to occur */
        /* Reminder: mutex is unlocket during wait but locked the mutex before
         * exiting */
        struct timespec ts;

        ts.tv_sec = waituntil.tv_sec;
        ts.tv_nsec = waituntil.tv_usec * 1000 + 1000;

        /* Reminder: mutex is unlocket during wait but locked the mutex before
         * exiting */
        pthread_cond_timedwait(&c->m_timer_cond, &c->m_SessionMutex, &ts);
#endif
    }
    bfcp_mutex_unlock(c->m_SessionMutex);
    c->Log(INF, "<< BFCPConnection: retransmission thread stopping. %p", c);
    return NULL;
}

#ifdef WIN32
unsigned __stdcall BFCPConnection::EntryPoint(void *pParam)
#else
void *BFCPConnection::EntryPoint(void *pParam)
#endif
{
    bool Status = true;
    if (pParam) {
        BFCPConnection *bfcpConnection = (BFCPConnection *)pParam;
        bfcpConnection->Log(INF,
                            ">> BFCPConnection: transport thread starting %p.", bfcpConnection);

        try {
            sockaddr_in out_addr;
            memset(&out_addr, 0, sizeof(sockaddr_in));
            bfcpConnection->m_Socket =
                bfcpConnection->m_remoteClient.CreateSocket();

            if (bfcpConnection->m_Socket != BFCP_INVALID_SOCKET) {
                bfcpConnection->Log(
                    INF, "BFCPConnection: created %s socket [%p]",
                    TRANSPORT_NAME(
                        bfcpConnection->m_remoteClient.GetTransport()),
                    bfcpConnection->m_Socket);
            } else {
                bfcpConnection->Log(
                    ERR,
                    "BFCPConnection: failed to create socket transport [%d]",
                    bfcpConnection->m_remoteClient.GetTransport());
                return NULL;
            }

            if (bfcpConnection->m_remoteClient.GetRole() ==
                    BFCPConnectionRole::PASSIVE &&
                bfcpConnection->m_remoteClient.GetTransport() !=
                    BFCP_OVER_UDP) {
                /* Listen */
                if (listen(bfcpConnection->m_Socket, SOMAXCONN) == -1) {
                    bfcpConnection->Log(
                        ERR,
                        "BFCPConnection: listen() to %s:%u failed, error: %s",
                        bfcpConnection->m_remoteClient.GetLocalAddr(),
                        bfcpConnection->m_remoteClient.GetLocalPort(),
                        GetErrorText().c_str());
                    Status = false;
                } else {
                    char ip[BFCP_STRING_SIZE] = {0};
                    int port = 0;
                    bfcpConnection->GetServerInfo(ip, &port);
                    bfcpConnection->Log(INF,
                                        "BFCPConnection: wait for incoming TCP "
                                        "on server socket [%d] at %s:%d",
                                        bfcpConnection->m_Socket, ip, port);
                    bfcpConnection->m_bConnected = true;
                }
            } else {
                if (Status) {
                    /* Connect to the Floor Control Server */
                    int connect_ret = bfcpConnection->m_remoteClient.Connect(
                            bfcpConnection->m_Socket);
                    if (connect_ret < 0) {
                        bfcpConnection->Log(ERR,
                                            "BFCP ACTIVE connection failed to "
                                            "connect to %s:%d, error: %d->%s",
                                            bfcpConnection->getRemoteAdress(),
                                            bfcpConnection->getRemotePort(),
                                            connect_ret,
                                            GetErrorText().c_str());
                        Status = false;
                    } else {
                        bfcpConnection->Log(
                            ERR,
                            "BFCP ACTIVE connection established with %s:%d",
                            bfcpConnection->getRemoteAdress(),
                            bfcpConnection->getRemotePort());

                        bfcpConnection->m_bConnected = true;
                        // Alert application
                        bfcpConnection->OnBFCPConnected(
                            bfcpConnection->m_Socket,
                            bfcpConnection->getRemoteAdress(),
                            bfcpConnection->getRemotePort());
                    }
                }
            }

            if (Status) {
                // enter transmission loop. Leave it on disconnect()
                bfcpConnection->RunLoop();
            }

        } catch (BFCPException &e) {
            bfcpConnection->Log(ERR, "BFCPConnection: %s", e.what());
        }

        bfcpConnection->m_isStarted = false;
        bfcpConnection->m_bConnected = false;

        bfcpConnection->Log(INF, "<< BFCP connection thread exiting");
    }
    return 0;
}

unsigned long BFCPConnection::availableBytes(BFCP_SOCKET p_sock) {
#ifndef WIN32
    int argp;
    if (0 == ioctl(p_sock, FIONREAD, &argp)) {
        return argp;
    }
#else

    DWORD argp;

    if (0 == ioctlsocket(p_sock, FIONREAD, &argp)) {
        /*		char c;
        if(-1 == m_pSocket->Recv(&c, 0)) {
        Log(INF,"Socket closed!\n");
        argp = -1;
        }
        */
        return argp;
    }
#endif
    return 0;
}

int BFCPConnection::Client2ServerInfo::Connect(BFCP_SOCKET s) {
    struct sockaddr *addr = (struct sockaddr *)&m_remoteAddress;
    if (addr->sa_family != AF_INET && addr->sa_family != AF_INET6) {
        return -2;
    }

    struct sockaddr_in *newAdd = (struct sockaddr_in *)addr;

    if (inet_pton(AF_INET, GetRemoteAddr(), &newAdd->sin_addr) <= 0) {
        return -3;
    }

    if (::connect(s, (struct sockaddr *)&m_remoteAddress, m_remoteAddrLen) ==
        -1) {
        return -1;
    } else {
        GetSockInfo(s);
        return 0;
    }
}

static void PrintAddress(const sockaddr *addr, std::string &addrstr,
                         UINT16 &p_port) {
    switch (addr->sa_family) {
        case AF_INET: {
            struct sockaddr_in *addrv4 = (struct sockaddr_in *)addr;
            char buf[INET_ADDRSTRLEN];
            addrstr = inet_ntop(addr->sa_family, &addrv4->sin_addr, buf,
                                INET_ADDRSTRLEN);
            p_port = ntohs(addrv4->sin_port);
            break;
        }

        case AF_INET6: {
            struct sockaddr_in6 *addrv6 = (struct sockaddr_in6 *)addr;
            char buf[INET6_ADDRSTRLEN];
            addrstr = inet_ntop(addr->sa_family, &addrv6->sin6_addr, buf,
                                INET6_ADDRSTRLEN);
            p_port = ntohs(addrv6->sin6_port);
            break;
        }

        default:
            break;
    }
}

bool BFCPConnection::Client2ServerInfo::SetAddress(const char *addrstr,
                                                   UINT16 port,
                                                   struct sockaddr *addr,
                                                   socklen_t &addrlen) {
    if (!addrstr) return false;
    /* Try IP V4 first */
    struct sockaddr_in *addrv4 = (struct sockaddr_in *)addr;
    if (inet_pton(AF_INET, addrstr, &(addrv4->sin_addr)) != 0) {

        addrlen = sizeof(struct sockaddr_in);
        addrv4->sin_port = htons(port);
        addr->sa_family = AF_INET;
        return true;
    }

    if (inet_pton(AF_INET6, addrstr, addr) == 1) {
        struct sockaddr_in6 *addrv6 = (struct sockaddr_in6 *)addr;

        addr->sa_family = AF_INET6;
        addrlen = sizeof(struct sockaddr_in6);
        addrv6->sin6_port = htons(port);
        return true;
    }
    return false;
}

bool BFCPConnection::Client2ServerInfo::SetLocalAddress(const char *addr,
                                                        UINT16 port) {
    if (SetAddress(addr, port, (struct sockaddr *)&m_localAddress, m_addrlen)) {
        m_localAddressStr = addr;
        m_localPort = port;
        return true;
    }
    return false;
}
void BFCPConnection::Client2ServerInfo::SetLocalAddress(sockaddr *addr,
                                                        socklen_t addrlen) {
    size_t len = (addrlen > 0 && addrlen <= sizeof(m_localAddress))
                     ? addrlen
                     : sizeof(m_remoteAddress);
    memcpy(&m_localAddress, addr, len);
    m_addrlen = addrlen;

    PrintAddress(addr, m_localAddressStr, m_localPort);
}

bool BFCPConnection::Client2ServerInfo::SetRemoteAddress(const char *addr,
                                                         UINT16 port) {
    // std::cout << __FUNCTION__ << addr << ":" << port << std::endl;
    if (SetAddress(addr, port, (struct sockaddr *)&m_remoteAddress,
                   m_remoteAddrLen)) {
        m_remoteAddressStr = addr;
        m_remotePort = port;
        return true;
    }
    return false;
}

void BFCPConnection::Client2ServerInfo::SetRemoteAddress(sockaddr *addr,
                                                         socklen_t addrlen) {
    size_t len = (addrlen > 0 && addrlen <= sizeof(m_remoteAddress))
                     ? addrlen
                     : sizeof(m_remoteAddress);
    memcpy(&m_remoteAddress, addr, len);
    m_remoteAddrLen = addrlen;

    PrintAddress(addr, m_remoteAddressStr, m_remotePort);
    std::ostringstream remoteAddressAndPort;
    switch (m_remoteAddress.ss_family) {
        case AF_INET:
            remoteAddressAndPort << m_remoteAddressStr << ':' << m_remotePort;
            break;

        case AF_INET6:
            remoteAddressAndPort << '[' << m_remoteAddressStr
                                 << "]:" << m_remotePort;
            break;

        default:
            break;
    }
    m_remoteAddressAndPort = remoteAddressAndPort.str();
    // std::cout << __FUNCTION__ << m_remoteAddressAndPort.c_str();
}

void BFCPConnection::RunLoop() {
    try {
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        fd_set fdset, allSet;

        FD_ZERO(&fdset);
        FD_ZERO(&allSet);
        FD_SET(m_Socket, &allSet);
#ifndef WIN32
        FD_SET(pipefd[0], &allSet);
#endif
        BFCP_SOCKET listenSocket = m_Socket;

        while (!m_bClose) {
            fdset = allSet;
            tv.tv_sec = 1;
            int nready = select((int)listenSocket + 1, &fdset, NULL, NULL, &tv);
            if (m_bClose || m_Socket == BFCP_INVALID_SOCKET) continue;

            // Log(INF, "BFCPConnection::transmitLoop nready=%d, m_Socket: %d, %p, role: %d\n",nready, m_Socket, this, m_eRole);
            if (nready < 0) {
                int err = errno;

                bfcp_mutex_lock(m_mutConnect);

                if (err == EBADF) {
                    std::map<BFCP_SOCKET, Client2ServerInfo>::iterator it;

                    Log(INF, "BFCPConnection: one client has been removed.");

                    listenSocket = m_Socket;
#ifndef WIN32
                    if (pipefd[0] > listenSocket) listenSocket = pipefd[0];
#endif

                    FD_ZERO(&allSet);
                    FD_SET(m_Socket, &allSet);
#ifndef WIN32
                    FD_SET(pipefd[0], &allSet);
#endif

                    for (it = m_ClientSocket.begin();
                         it != m_ClientSocket.end() && !m_bClose; it++) {
                        FD_SET(it->first, &allSet);
                        if (it->first > listenSocket) listenSocket = it->first;
                    }

                    bfcp_mutex_unlock(m_mutConnect);
                    continue;
                }

                Log(INF,
                    "BFCPConnection::RunLoop %s:%d select failed. errno=%d",
                    getLocalAdress(), getLocalPort(), err);

                if (m_Socket != BFCP_INVALID_SOCKET) {
                    FD_CLR(m_Socket, &allSet);
                    m_remoteClient.CloseSocket(m_Socket);
                }

                m_bClose = true;
                bfcp_mutex_unlock(m_mutConnect);
                OnBFCPDisconnected(m_Socket);
                m_Socket = BFCP_INVALID_SOCKET;
                break;
            }

            if (nready >= 0) {
                /* there are data to process or connection to handle */
                std::map<BFCP_SOCKET, Client2ServerInfo>::iterator it;
                int ret;
                bool disconnect = false;
                BFCP_SOCKET s;

#ifndef WIN32
                if (FD_ISSET(pipefd[0], &fdset)) {
                    char bufpipe[2];
                    if (read(pipefd[0], bufpipe, 2) < 0)
                        Log(ERR,
                            "BFCPConnection::RunLoop - could not write to "
                            "pipe");
                    // Log(INF, "BFCPConnection::RunLoop - control pipe
                    // signal");
                }
#endif

                if (m_remoteClient.CheckExpiredAnswers(this) < 0) {
                    /* Main socket has expired GoodByeAck -> should close */
                    m_remoteClient.CloseSocket(m_Socket);
                    OnBFCPDisconnected(m_Socket);
                    m_bClose = true;
                }

                if (FD_ISSET(m_Socket, &fdset)) {
                    /* main socket has someting to say. Check if we are a TCP
                     * server or if we are running an UDP connection or a TCP
                     * active connection
                     */
                    if (m_eRole == BFCPConnectionRole::ACTIVE ||
                        m_remoteClient.GetTransport() == BFCP_OVER_UDP) {
                        ret = m_remoteClient.ReadData(this, m_Socket);

                        if (ret == 1 && m_remoteClient.parsed_msg != NULL) {
                            if (m_remoteClient.GetTransport() ==
                                BFCP_OVER_UDP) {
                                int retClose = CloseOutgoingTransaction(
                                    m_Socket, m_remoteClient.message);
                                Log(INF, "Closed transaction %i", retClose);
                                if (!m_remoteClient.HandleRemoteRetrans(
                                        this, m_Socket,
                                        m_remoteClient.message)) {
                                    ProcessBFCPmessage(
                                        m_remoteClient.parsed_msg, m_Socket);
                                }
                            } else {
                                ProcessBFCPmessage(m_remoteClient.parsed_msg,
                                                   m_Socket);
                            }
                            m_remoteClient.CleanupRead();
                        } else if (ret == -3) {
                            /* transport error on main socket - shutdown all
                             * server */
                            if (!m_bClose) {
                                OnBFCPDisconnected(m_Socket);
                                m_remoteClient.CloseSocket(m_Socket);
                                m_bClose = true;
                            }
                            m_Socket = BFCP_INVALID_SOCKET;
                            break;
                        }
                    } else {
                        /* Handle incoming TCP connection */
                        struct sockaddr_storage out_addr;
#ifdef WIN32
                        int addrlen;
#else
                        socklen_t addrlen;
#endif
                        memset(&out_addr, 0, sizeof(out_addr));
                        addrlen = sizeof(out_addr);
                        Client2ServerInfo c2s(BFCP_OVER_TCP);
                        BFCP_SOCKET acceptSocket =
                            accept(m_Socket, (sockaddr *)&out_addr, &addrlen);
                        if (acceptSocket != BFCP_INVALID_SOCKET) {
                            // c2s.SetRemoteAddress(&out_addr, addrlen);
                            c2s.GetSockInfo(acceptSocket);

                            Log(INF,
                                "BFCPConnection::RunLoop PASSIVE incoming TCP "
                                "connection %s:%d <=> %s. nbclient=[%d], socket=[%d]",
                                getLocalAdress(), getLocalPort(),
                                c2s.GetRemoteAddrAndPort(),
                                m_ClientSocket.size() + 1,
                                acceptSocket);

                            bfcp_mutex_lock(m_mutConnect);
                            // m_ClientSocket[acceptSocket] = c2s ;
                            m_ClientSocket.insert(
                                std::pair<BFCP_SOCKET, Client2ServerInfo>(
                                    acceptSocket, c2s));
                            FD_SET(acceptSocket, &allSet);
                            bfcp_mutex_unlock(m_mutConnect);

                            if (acceptSocket > listenSocket)
                                listenSocket = acceptSocket;

                            // Alert application
                            if (!m_bClose)
                                OnBFCPConnected(acceptSocket,
                                                c2s.GetRemoteAddr(),
                                                c2s.GetRemotePort());
                        }
                    }
                }

                /*  Now process data on all connected clients */
                bfcp_mutex_lock(m_mutConnect);
                s = BFCP_INVALID_SOCKET;

                for (it = m_ClientSocket.begin();
                     it != m_ClientSocket.end() && !m_bClose; it++) {
                    s = it->first;

                    /* Pay Attention:
                     * Resync allSet in case some fd are missing if AddClient()
                     * has been called before starting the RunLoop() 
                     */
                    if (!FD_ISSET(s, &allSet)) {
                        FD_SET(s, &allSet);
                        if (s > listenSocket) listenSocket = s;
                        Log(INF,
                            "BFCPConnection::RunLoop - adding new client fd "
                            "[%d], maxfdval = [%d]",
                            s, listenSocket);
                    }

                    /* UDP: cleanup expired BFCP answer (retransmission
                     * handling) */
                    if (it->second.CheckExpiredAnswers(this) < 0) {
                        disconnect = true;
                        break;
                    }

                    /* If no data is ready, we are finised with this client */
                    if (!FD_ISSET(s, &fdset)) continue;

                    // debug traces
                    // Log(INF, "BFCPConnection::RunLoop - we have data to read on %s socket FD [%d]",
                    //    TRANSPORT_NAME(it->second.GetTransport()), s);

                    ret = it->second.ReadData(this, s);
                    if (ret == 1 && it->second.parsed_msg != NULL) {
                        if (it->second.GetTransport() == BFCP_OVER_UDP) {
                            if (CloseOutgoingTransaction(
                                    s, it->second.message) == 1) {
                                Log(INF, "Closed transaction %u",
                                    it->second.parsed_msg->entity
                                        ->transactionID);
                            }

                            if (!it->second.HandleRemoteRetrans(
                                    this, s, it->second.message)) {
                                ProcessBFCPmessage(it->second.parsed_msg, s);
                                if (it->second.parsed_msg->primitive ==
                                    e_primitive_GoodbyeAck) {
                                    /* We 've receive a GoodbyeAck so we need to
                                     * close everything */
                                    Log(INF,
                                        "BFCPConnection: received a GoodByeAck "
                                        "from %s:%d - we're on UDP - this is a "
                                        "disconnect !",
                                        getLocalAdress(), getLocalPort(),
                                        it->second.GetRemoteAddr(),
                                        it->second.GetRemotePort());
                                    disconnect = true;
                                    break;
                                }
                            }
                        } else {
                            Log(INF,
                                "BFCPConnection::RunLoop PASSIVE process BFCP message "
                                "connection %s:%d <=> %s. nbclient=[%d], socket=[%d]",
                                getLocalAdress(), getLocalPort(),
                                it->second.GetRemoteAddrAndPort(),
                                m_ClientSocket.size() + 1,
                                s);
                            ProcessBFCPmessage(it->second.parsed_msg, s);
                        }

                        it->second.CleanupRead();
                    } else if (ret == -3) {
                        /* transport error on client socket - remove it from
                         * list */
                        Log(INF,
                            "BFCPConnection::RunLoop Connection %s:%d <=> "
                            "%s:%d lost !",
                            getLocalAdress(), getLocalPort(),
                            it->second.GetRemoteAddr(),
                            it->second.GetRemotePort());

                        disconnect = true;
                        break;
                    }
                }

                if (disconnect) {
                    /* remove disconnected socket from set and client list */
                    m_ClientSocket.erase(s);  // Remove from list
                    FD_CLR(s, &allSet);       // and from set

                    /* Recompute last FD for select */
                    listenSocket = m_Socket;
#ifndef WIN32
                    if (pipefd[0] > listenSocket) listenSocket = pipefd[0];
#endif

                    for (it = m_ClientSocket.begin();
                         it != m_ClientSocket.end() && !m_bClose; it++) {
                        if (it->first > listenSocket) listenSocket = it->first;
                    }
                    bfcp_mutex_unlock(m_mutConnect);
                    if (!m_bClose) OnBFCPDisconnected(s);
                } else {
                    bfcp_mutex_unlock(m_mutConnect);
                }
            }
        } /* while */
    } catch (...) {
        Log(ERR, "Exception catched in transmit loop!");
    }
    Log(INF, "Closed");
}

BFCP_SOCKET BFCPConnection::Client2ServerInfo::CreateSocket() {
    BFCP_SOCKET fd = BFCP_INVALID_SOCKET;
    struct sockaddr *addr = (struct sockaddr *)&m_localAddress;
    char msg[200];
    if (addr->sa_family != AF_INET && addr->sa_family != AF_INET6) {
        /* Log(ERR, "Unsupported network protocol specified for local
         * address."); */
        throw BFCPException(
            "BFCPConnection", __LINE__, "Network protocol",
            "Unsupported network protocol specified for local address");
    }

    /* Setup the socket-related stuff */
    switch (GetTransport()) {
        case BFCP_OVER_TCP:
            fd = socket(addr->sa_family, SOCK_STREAM, 0);
            break;

        case BFCP_OVER_TLS:
            /* Shoudn't we use OpenSSL here ? */
            fd = socket(addr->sa_family, SOCK_STREAM, 0);
            break;

        case BFCP_OVER_UDP:
            fd = socket(addr->sa_family, SOCK_DGRAM, 0);
            break;

        default:
            sprintf(msg, "unsupported transport protocol %d", GetTransport());
            throw BFCPException("BFCPConnection", __LINE__,
                                "Transport protocol", msg);
            break;
    }

    if (fd != BFCP_INVALID_SOCKET) {
        if (GetTransport() != BFCP_OVER_UDP) {
            int yes = 1;
#ifndef WIN32
            if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0)
#else
            if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&yes,
                           sizeof(int)) < 0)
#endif
            {
                CloseSocket(fd);
                sprintf(msg, "failed to set REUSEADDR on socket [%d]. errno=%d",
                        (int)fd, errno);
                throw BFCPException("BFCPConnection", __LINE__,
                                    "Transport protocol", msg);
            }

#ifdef WIN32
            yes = 0;
            if (setsockopt(fd, SOL_SOCKET, SO_LINGER, (char *)&yes,
                           sizeof(int)) < 0) {
                CloseSocket(fd);
                sprintf(msg, "failed to set SO_LINGER on socket [%d]. errno=%d",
                        (int)fd, errno);
                throw BFCPException("BFCPConnection", __LINE__,
                                    "Transport protocol", msg);
            }
#endif

            yes = 1;
#ifndef WIN32
            // setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(int));
            struct KeepConfig {
                /** The time (in seconds) the connection needs to remain
                 * idle before TCP starts sending keepalive probes (TCP_KEEPIDLE
                 * socket option)
                 */
                int keepidle;
                /** The maximum number of keepalive probes TCP should
                 * send before dropping the connection. (TCP_KEEPCNT socket
                 * option)
                 */
                int keepcnt;

                /** The time (in seconds) between individual keepalive probes.
                 *  (TCP_KEEPINTVL socket option)
                 */
                int keepintvl;
            };
            struct KeepConfig cfg = {60, 10, 10};
            // set the keepalive options
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cfg.keepcnt,
                       sizeof cfg.keepcnt);
#ifdef TCP_KEEPIDLE
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &cfg.keepidle,
                       sizeof cfg.keepidle);
#endif
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &cfg.keepintvl,
                       sizeof cfg.keepintvl);
            sprintf(msg, "Keep alive idle_time=%d, keepcnt=%d and keepintvl=%d",
                    cfg.keepidle, cfg.keepcnt, cfg.keepintvl);
#else
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char *)&yes, sizeof(int));
#endif
        }

        if (GetTransport() == BFCP_OVER_UDP ||
            m_role == BFCPConnectionRole::PASSIVE) {
            /* Bind the socket to the local address */
            if (bind(fd, addr, m_addrlen) < 0) {
                CloseSocket(fd);
                sprintf(msg,
                        "failed to bind() socket [%d] to %s : %d - error: %s",
                        (int)fd, GetLocalAddr(), GetLocalPort(),
                        GetErrorText().c_str());
                throw BFCPException("Client2ServerInfo", __LINE__,
                                    "Transport protocol", msg);
            }

#ifndef WIN32
            if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1)
#else
            unsigned long NonBlock = 1;
            if (ioctlsocket(fd, FIONBIO, &NonBlock) != 0)
#endif
            {
                CloseSocket(fd);
                sprintf(
                    msg,
                    "failed to set socket [%d] in non blocking mode errno=%d",
                    (int)fd, errno);
                throw BFCPException("BFCPConnection", __LINE__,
                                    "Transport protocol", msg);
            }
        }

    } else {
        sprintf(msg, "failed to create socket family=%d transport=%d. errno=%d",
                addr->sa_family, m_bfcp_transport, errno);
        throw BFCPException("BFCPConnection", __LINE__, "Transport protocol",
                            msg);
    }

    return fd;
}

static bool CompareAddresses(const struct sockaddr *a1,
                             const struct sockaddr *a2) {
    if (a1->sa_family != a2->sa_family) return false;

    switch (a1->sa_family) {
        case AF_INET: {
            const struct sockaddr_in *a1v4 = (const struct sockaddr_in *)a1;
            const struct sockaddr_in *a2v4 = (const struct sockaddr_in *)a2;
            if (a1v4->sin_port != a2v4->sin_port) break;
            if (a1v4->sin_addr.s_addr != a2v4->sin_addr.s_addr) break;
            return true;
        }

        case AF_INET6: {
            const struct sockaddr_in6 *a1v6 = (const struct sockaddr_in6 *)a1;
            const struct sockaddr_in6 *a2v6 = (const struct sockaddr_in6 *)a2;
            if (a1v6->sin6_port != a2v6->sin6_port) break;
            if (memcmp(&a1v6->sin6_addr, &a2v6->sin6_addr,
                       sizeof(a1v6->sin6_addr)) != 0)
                return false;
            return true;
        }

        default:
            break;
    }
    return false;
}

void BFCPConnection::Client2ServerInfo::Init() {
    struct sockaddr_in *addr = (struct sockaddr_in *)&m_localAddress;
    m_remotePort = 0;
    message = NULL;
    recvidx = 0;
    msgsize = 0;
    parsed_msg = NULL;

    // memset(recvBuffer, 0, sizeof(recvBuffer));
    memset(&m_localAddress, 0, sizeof(m_localAddress));
    memset(&m_remoteAddress, 0, sizeof(m_remoteAddress));

    /* Set default IP address and port */
    m_localAddress.ss_family = AF_INET;
    addr->sin_port = htons(BFCP_FCS_DEFAULT_PORT);
    addr->sin_addr.s_addr = INADDR_ANY;
    m_addrlen = sizeof(struct sockaddr_in);

    m_remoteAddressAndPort.clear();
    m_remoteAddressStr.clear();
    m_localAddressStr.clear();

    PrintAddress((struct sockaddr *)addr, m_localAddressStr, m_localPort);
}

bool BFCPConnection::Client2ServerInfo::GetSockInfo(BFCP_SOCKET s) {
    bool Status = true;
    struct sockaddr_storage sa;
    socklen_t sa_len = sizeof(sa);

    if (s == BFCP_INVALID_SOCKET) return false;

    if (getsockname(s, (sockaddr *)&sa, &sa_len) == 0) {
        memcpy(&m_localAddress, &sa, sa_len);
        PrintAddress((sockaddr *)&sa, m_localAddressStr, m_localPort);
    } else {
        Status = false;
    }

    if (GetTransport() == BFCP_OVER_TCP) {
        if (getpeername(s, (sockaddr *)&sa, &sa_len) == 0) {
            SetRemoteAddress((sockaddr *)&sa, sa_len);
        } else {
            Status = false;
        }
    }

    return Status;
}

void BFCPConnection::Client2ServerInfo::CleanupRead() {
    recvidx = 0;
    msgsize = 0;
    if (message) {
        bfcp_free_message(message);
        message = NULL;
    }
}

int BFCPConnection::Client2ServerInfo::ReadData(BFCPConnection *c,
                                                BFCP_SOCKET s) {
    int error = 0;
    int toread = 0;
    // bfcp_message *message = NULL;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    switch (GetTransport()) {
        case BFCP_OVER_UDP:
            error = recvfrom(s, (char *)recvBuffer, BFCP_MAX_ALLOWED_SIZE, 0,
                             (struct sockaddr *)&addr, &addrlen);
            if (error >= 0) recvidx = error;

            if (error == 0) {
                c->Log(ERR, "BFCP UDP connection [%d] invalid.", s);
                CleanupRead();
                return -3;
            }

            if (error < 0) {
                c->Log(ERR,
                       "BFCP UDP connection read data error on fd [%d]. Errno "
                       "= %d - %s",
                       s, errno, GetErrorText().c_str());
                goto transport_read_error;
            }

            /* Check IP addresses */
            if (!m_remoteAddressAndPort.empty()) {
                if (!CompareAddresses((const struct sockaddr *)&m_remoteAddress,
                                      (const struct sockaddr *)&addr)) {
                    c->Log(ERR,
                           "BFCP connection [%d]: dropped packet received from "
                           "another IP address. %s; local is %s:%d and remote is expected to be: %s",
                           s, GetRemoteAddrAndPort(), GetLocalAddr(), m_localPort, GetRemoteAddrAndPort());
                    // TODO this is very dangerous, but our current implementation need it, let's fix it later.
                    SetRemoteAddres(&addr, sizeof(const struct sockaddr));
                    //return -2;
                }
            }
            break;

        case BFCP_OVER_TCP:
            if (recvidx < 12) {
                // Need to read header
                toread = 12 - recvidx;
            } else {
                toread = msgsize - recvidx;
            }

            error = recv(s, (char *)(recvBuffer + recvidx), toread, 0);

            if (error < 0) {
                c->Log(ERR,
                       "BFCP TCP connection read data error on fd [%d]. Errno "
                       "= %d - %s",
                       s, errno, GetErrorText().c_str());
                goto transport_read_error;
            } else if (error == 0) {
                c->Log(ERR,
                       "BFCP connection [%d] closed. Nb of bytes expected %d",
                       s, toread);
                CleanupRead();
                return -3;
            }
            recvidx += error;
            break;

        default:
            c->Log(ERR, "BFCP unsupported transport %d for socket [%d].",
                   GetTransport(), s);
            return -3;
    }

    if (msgsize == 0 && recvidx >= 12) {
        message = bfcp_new_message(recvBuffer, recvidx);
        int msgz = bfcp_get_length(message);
        if (msgz < 0) {
            c->Log(ERR, "BFCP parse header error: invalid payload length.");
            CleanupRead();
            return -1;
        }

        msgsize = bfcp_get_length(message) * 4 + 12;

        if (msgsize > BFCP_MAX_ALLOWED_SIZE) {
            c->Log(ERR, "BFCP message too big. Discarding");
            CleanupRead();
            return -2;
        }
    }

    if (msgsize > 0 && recvidx >= msgsize) {
        /* We have enough data - parse the whole thing*/
        if (message != NULL) bfcp_free_message(message);
        message = bfcp_new_message(recvBuffer, recvidx);
        if (!message) {
            /* Failed to allocate */
            CleanupRead();
            return -1;
        }

        parsed_msg = bfcp_parse_message(message);
        if (!parsed_msg) {
            /* Failed to parse */
            CleanupRead();
            c->Log(ERR, "BFCP failed to parse incoming message on socket [%d].",
                   s);
            return -1;
        }

        /* Message correctly parsed */
        if (GetTransport() == BFCP_OVER_UDP)
            SetRemoteAddress((struct sockaddr *)&addr, addrlen);
        parsed_msg->transport = GetTransport();
        return 1;
    }

    if (GetTransport() == BFCP_OVER_UDP) {
        c->Log(ERR, "BFCP message incomplete. Discarding");
        CleanupRead();
        return -2;
    }

    /* Need more data (TCP or TLS)*/
    return 0;

transport_read_error:
    CloseSocket(s);
    CleanupRead();
    return -3;
}

#define TCP_CHUNK 1400

int BFCPConnection::Client2ServerInfo::SendData(BFCPConnection *c,
                                                BFCP_SOCKET s,
                                                bfcp_message *msg) {
    int ret;

    if (s == BFCP_INVALID_SOCKET) {
        c->Log(ERR, "Cannot send data. Invalid socket");
        return -1;
    }

    if (GetTransport() == BFCP_OVER_UDP) {
        UINT16 trID = bfcp_get_transactionID(msg);

        if (m_remoteAddressAndPort.empty()) {
            c->Log(ERR, "UDP/BFCP Could not send msg: no destination address.");
            // return -2; //Don't need to return in case of UDP
        }

        ret = sendto(s, (const char *)msg->buffer, msg->length, 0,
                     (struct sockaddr *)&m_remoteAddress, m_addrlen);
        if (ret == -1) {
            c->Log(ERR, "UDP/BFCP message sending failed. errno=%d", errno);
            return -3;
        }

        c->Log(ERR, "FD %d Sent data to %s:%d", s, GetRemoteAddr(),
               m_remotePort);
        /* record answer sent */
        if (trID > 0) {
            Transaction t(s, msg);
            answerMap[trID] = t;
        }
    } else {
        int total = 0; /* How many bytes have been sent so far */
        // msg->length;	/* How many bytes still have to be sent */

        /* Wait up to ten seconds before timeout */
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(s, &wset);
        int error;

        while (total < msg->length) {
            error = select(s + 1, NULL, &wset, NULL, &tv);
            if (error < 0) return -3; /* Select error */

            if (error == 0) {
                c->Log(ERR, "TCP/BFCP Could not send msg: timeout.");
                return -2; /* Timeout */
            }

            if (FD_ISSET(s, &wset)) {
                int len = (msg->length - total > TCP_CHUNK)
                              ? TCP_CHUNK
                              : msg->length - total;

                error = send(s, (char *)msg->buffer + total, len, 0);
                if (error < 0) { /* Error sending the message */
                    c->Log(ERR, "UDP/BFCP message sending failed. errno=%d",
                           errno);
                    return -3;
                }
                total += error; /* Update the sent and to-be-sent bytes */
            }
        }
    }
    return 0;
}

int BFCPConnection::Client2ServerInfo::CloseSocket(BFCP_SOCKET s) {
    if (s == BFCP_INVALID_SOCKET) return -1;

    // Log(INF, "BFCP shutdown socket[0x%p] ",s);
    shutdown(s, 2);
    return closesocket(s);
}

bool BFCPConnection::GetServerInfo(char *localIp, int *localPort) {
    *localPort = m_remoteClient.GetLocalPort();
    strcpy(localIp, m_remoteClient.GetLocalAddr());
    return true;
}

BFCP_SOCKET BFCPConnection::AddClient(int transport, int role,
                                      char *localAddress, UINT16 port) {
    BFCP_SOCKET fd = BFCP_INVALID_SOCKET;
    const char *addr;

    /* Only BFCP over UDP is supported for now */
    if (transport != BFCP_OVER_UDP) {
        Log(ERR,
            "BFCPConnection: Failed to add client: unsupported transport %s.",
            TRANSPORT_NAME(transport));
        return BFCP_INVALID_SOCKET;
    }

    if (localAddress == NULL || localAddress[0] == 0)
        addr = getLocalAdress();
    else
        addr = localAddress;

    Client2ServerInfo c2s(transport, role);
    if (c2s.SetLocalAddress(addr, port)) {
        try {
            fd = c2s.CreateSocket();
            if (fd != BFCP_INVALID_SOCKET) {
                Log(INF, "AddClient: openened socket [%d]", fd);
                bfcp_mutex_lock(m_mutConnect);
                m_ClientSocket[fd] = c2s;
                // m_ClientSocket.insert(
                // std::pair<BFCP_SOCKET,Client2ServerInfo>(fd,c2s) );
                bfcp_mutex_unlock(m_mutConnect);

#ifndef WIN32
                /* This will unblock the select in RunLoop ! */
                if (write(pipefd[1], "ok", 2) < 0) {
                    Log(INF,
                        "BFCPConnection: failed to signal the RunLoop for a "
                        "new client");
                }
#endif

                if (localAddress != NULL && localAddress[0] == 0) {
                    strcpy(localAddress, c2s.GetLocalAddr());
                }

                Log(INF,
                    "BFCPConnection: Added client %s connection on %s : %d -> "
                    "fd=[%d]",
                    TRANSPORT_NAME(transport), addr, port, fd);
            } else {
                Log(ERR,
                    "BFCPConnection: Failed to add client %s connection on %s "
                    ": %d. Could not create or bind socket.",
                    TRANSPORT_NAME(transport), addr, port);
            }
        } catch (BFCPException &e) {
            Log(ERR, "BFCPConnection: %s", e.what());
        }
    } else {
        Log(ERR, "BFCPConnection: invalid local address [%s].", addr);
    }
    return fd;
}

bool BFCPConnection::RemoveClient(BFCP_SOCKET s) {
    if (s != BFCP_INVALID_SOCKET) {
        bool lock = false;

        if (BFCP_CURRENT_THREAD() != m_thread) {
            bfcp_mutex_lock(m_mutConnect);
            lock = true;
        }

        if (m_ClientSocket.erase(s) > 0) {
            Client2ServerInfo::CloseSocket(s);
        }

        if (lock) bfcp_mutex_unlock(m_mutConnect);
        return true;
    }
    return false;
}
BFCPConnection::Transaction::Transaction(BFCP_SOCKET s, bfcp_message *m)
    : m_sockfd(s) {
    if (!m)
        message = NULL;
    else
        message = bfcp_copy_message(m);

    gettimeofday(&timerExpiration, 0);
    timerExpiration.tv_usec += 500 * 1000;
    timerDuration = 500;
    m_sockfd = s;
}

BFCPConnection::Transaction::Transaction() : m_sockfd(BFCP_INVALID_SOCKET) {
    message = NULL;
    timerExpiration.tv_sec = 0;
    timerExpiration.tv_usec = 0;
    timerDuration = 1;
}

BFCPConnection::Transaction::~Transaction() {
    if (message) bfcp_free_message(message);
}

BFCPConnection::Transaction &BFCPConnection::Transaction::operator=(
    const Transaction &other) {
    if (!other.message)
        message = NULL;
    else
        message = bfcp_copy_message(other.message);
    timerExpiration = other.timerExpiration;
    timerDuration = other.timerDuration;
    m_sockfd = other.m_sockfd;
    return *this;
}

bool BFCPConnection::GetConnectionLocalInfo(BFCP_SOCKET s, char *localIp,
                                            int *localPort) {
    std::map<BFCP_SOCKET, Client2ServerInfo>::iterator it;

    if (s == BFCP_INVALID_SOCKET) {
        Log(ERR, "BFCPConnection: invalid file descriptor for socket.");
        return false;
    }

    if (s == m_Socket) {
        Log(ERR, "BFCPConnection: providing server socket information");
        if (localIp) strcpy(localIp, m_remoteClient.GetLocalAddr());
        if (localPort) *localPort = m_remoteClient.GetLocalPort();
        return true;
    }

    bfcp_mutex_lock(m_mutConnect);
    it = m_ClientSocket.find(s);
    if (it == m_ClientSocket.end()) {
        bfcp_mutex_unlock(m_mutConnect);
        Log(ERR,
            "BFCPConnection: cannot find client connection associated with "
            "fd=[%d]",
            s);
        return false;
    }

    if (localIp != NULL) strcpy(localIp, it->second.GetLocalAddr());
    if (localPort != NULL) {
        *localPort = it->second.GetLocalPort();
        Log(INF, "BFCPConnection: sockfd [%d]-> local port %u, transport %s", s,
            *localPort, TRANSPORT_NAME(it->second.GetTransport()));
    }
    bfcp_mutex_unlock(m_mutConnect);
    return true;
}
