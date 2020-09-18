#ifndef BACKEND_H
#define BACKEND_H

#include "EpollObject.hpp"
#include <netinet/in.h>
#include <unordered_map>
#include <vector>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <iostream>

class Http;

//to debug
#define MAXBACKEND 1
//#define MAXBACKEND 3

class Backend : public EpollObject
{
public:
    struct BackendList
    {
        std::vector<Backend *> busy;
        std::vector<Backend *> idle;
        std::vector<Http *> pending;//only when no idle and max busy reached
        sockaddr_in6 s;
    };
    enum NonHttpError : uint8_t
    {
        NonHttpError_AlreadySend
    };
public:
    Backend(BackendList * backendList);
    virtual ~Backend();
    void remoteSocketClosed();
    static Backend * tryConnectInternalList(const sockaddr_in6 &s, Http *http, std::unordered_map<std::string, BackendList *> &addressToList, bool &connectInternal);
    static Backend * tryConnectHttp(const sockaddr_in6 &s,Http *http, bool &connectInternal);
    static Backend * tryConnectHttps(const sockaddr_in6 &s,Http *http, bool &connectInternal);
    void startHttps();
    void downloadFinished();
    void parseEvent(const epoll_event &event) override;
    bool tryConnectInternal(const sockaddr_in6 &s);
    static std::unordered_map<std::string,BackendList *> addressToHttp;
    static std::unordered_map<std::string,BackendList *> addressToHttps;

    void readyToWrite();
    ssize_t socketRead(void *buffer, size_t size);
    bool socketWrite(const void *buffer, size_t size);
    #ifdef DEBUGHTTPS
    static void dump_cert_info(SSL *ssl, bool server);
    #endif
public:
    Http *http;
    bool https;
    bool wasTCPConnected;
private:
    std::string bufferSocket;
    BackendList * backendList;

    const SSL_METHOD *meth;
    SSL_CTX* ctx;
    SSL* ssl;
};

#endif // BACKEND_H
