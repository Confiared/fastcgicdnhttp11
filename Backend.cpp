#include "Backend.hpp"
#include "Http.hpp"
#include "Cache.hpp"
#include <iostream>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <sys/time.h>
#include <sys/stat.h>

#ifdef DEBUGFASTCGI
#include <sys/time.h>
#endif

//curl -v -H "Accept-Encoding: gzip" -o style.css.gz 'http://cdn.bolivia-online.com/ultracopier-static.first-world.info/css/style.css'

std::unordered_map<std::string,Backend::BackendList *> Backend::addressToHttp;
std::unordered_map<std::string,Backend::BackendList *> Backend::addressToHttps;
uint32_t Backend::maxBackend=64;

uint16_t Backend::https_portBE=0;

Backend::Backend(BackendList * backendList) :
    http(nullptr),
    https(false),
    wasTCPConnected(false),
    lastReceivedBytesTimestamps(0),
    backendList(backendList),
    meth(nullptr),
    ctx(nullptr),
    ssl(nullptr)
{
    lastReceivedBytesTimestamps=Backend::currentTime();
    this->kind=EpollObject::Kind::Kind_Backend;

    #ifdef MAXFILESIZE
    fileGetCount=0;
    byteReceived=0;
    #endif
}

Backend::~Backend()
{
    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    if(fd!=-1)
    {
        std::cerr << "EPOLL_CTL_DEL Http: " << fd << std::endl;
        Cache::closeFD(fd);
        if(epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL)==-1)
            std::cerr << "EPOLL_CTL_DEL Http: " << fd << ", errno: " << errno << std::endl;
    }
    if(http!=nullptr)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << http << ": http->backend=nullptr; (destructor)" << std::endl;
        #endif
        http->backend=nullptr;
        http=nullptr;
    }
    if(backendList!=nullptr)
    {
        size_t index=0;
        while(index<backendList->busy.size())
        {
            if(backendList->busy.at(index)==this)
            {
                backendList->busy.erase(backendList->busy.cbegin()+index);
                break;
            }
            index++;
        }
        index=0;
        while(index<backendList->idle.size())
        {
            if(backendList->idle.at(index)==this)
            {
                backendList->idle.erase(backendList->idle.cbegin()+index);
                break;
            }
            index++;
        }
    }
    closeSSL();
}

void Backend::close()
{
    #ifdef DEBUGFASTCGI
    std::cerr << "Backend::close() " << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    if(fd!=-1)
    {
        Cache::closeFD(fd);
        epoll_ctl(epollfd,EPOLL_CTL_DEL, fd, NULL);
        ::close(fd);
        //prevent multiple loop call
        fd=-1;
    }
    closeSSL();
}

void Backend::closeSSL()
{
    if(ssl!=nullptr)
    {
        SSL_free(ssl);
        ssl=nullptr;
    }
    meth = TLS_client_method();
    if(meth!=nullptr)
    {
        delete meth;
        meth=nullptr;
    }
    if(ctx!=NULL)
    {
        SSL_CTX_free(ctx);
        ctx=nullptr;
    }
}

void Backend::remoteSocketClosed()
{
    #ifdef DEBUGFASTCGI
    std::cerr << "Backend::remoteSocketClosed() " << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    #ifdef DEBUGFILEOPEN
    std::cerr << "Backend::remoteSocketClosed(), fd: " << fd << std::endl;
    #endif
    if(fd!=-1)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << "EPOLL_CTL_DEL remoteSocketClosed Http: " << fd << std::endl;
        #endif
        Cache::closeFD(fd);
        if(epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL)==-1)
            std::cerr << "EPOLL_CTL_DEL remoteSocketClosed Http: " << fd << ", errno: " << errno << std::endl;
        ::close(fd);
        fd=-1;
    }
    closeSSL();
    if(http!=nullptr)
        http->resetRequestSended();
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    if(backendList!=nullptr)
    {
        if(!wasTCPConnected)
        {
            size_t index=0;
            while(index<backendList->busy.size())
            {
                if(backendList->busy.at(index)==this)
                {
                    backendList->busy.erase(backendList->busy.cbegin()+index);
                    break;
                }
                index++;
            }
            if(!backendList->pending.empty() && backendList->busy.empty())
            {
                const std::string error("Tcp connect problem");
                size_t index=0;
                while(index<backendList->pending.size())
                {
                    Http *http=backendList->pending.at(index);
                    http->backendError(error);
                    http->disconnectFrontend();
                    http->disconnectBackend();
                    index++;
                }
            }
            if(http!=nullptr)
                http->backend=nullptr;
            #ifdef DEBUGFASTCGI
            std::cerr << "remoteSocketClosed and was NOT TCP connected " << __FILE__ << ":" << __LINE__ << std::endl;
            #endif
            return;
        }
        else
        {
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " was TCP connected" << std::endl;
            #endif
            size_t index=0;
            while(index<backendList->busy.size())
            {
                if(backendList->busy.at(index)==this)
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " located into busy to destroy" << std::endl;
                    #endif
                    backendList->busy.erase(backendList->busy.cbegin()+index);
                    if(http!=nullptr)
                    {
                        #ifdef DEBUGFASTCGI
                        std::cerr << __FILE__ << ":" << __LINE__ << " backend destroy but had http client connected, try reasign" << std::endl;
                        #endif
                        /*if(http->requestSended)
                        {
                            std::cerr << "reassign but request already send" << std::endl;
                            http->parseNonHttpError(Backend::NonHttpError_AlreadySend);
                            return;
                        }*/
                        #ifdef DEBUGFASTCGI
                        if(http->requestSended)
                            std::cerr << "reassign but request already send" << std::endl;
                        #endif
                        http->requestSended=false;
                        //reassign to idle backend
                        if(!backendList->idle.empty())
                        {
                            //assign to idle backend and become busy
                            Backend *backend=backendList->idle.back();
                            backendList->idle.pop_back();
                            backendList->busy.push_back(backend);
                            backend->http=http;
                            #ifdef DEBUGFASTCGI
                            std::cerr << http << ": http->backend=" << backend << " " << __FILE__ << ":" << __LINE__ << " isValid: " << backend->isValid() << std::endl;
                            #endif
                            http->backend=backend;
                            http->readyToWrite();
                        }
                        //reassign to new backend
                        else
                        {
                            Backend *newBackend=new Backend(backendList);
                            if(!newBackend->tryConnectInternal(backendList->s))
                                //todo abort client
                                return;
                            newBackend->http=http;
                            #ifdef DEBUGFASTCGI
                            std::cerr << http << ": http->backend=" << newBackend << " " << __FILE__ << ":" << __LINE__ << " isValid: " << newBackend->isValid() << std::endl;
                            #endif
                            http->backend=newBackend;

                            backendList->busy.push_back(newBackend);
                        }
                        http=nullptr;
                        return;
                    }
                    if(backendList->busy.empty() && backendList->idle.empty() && backendList->pending.empty())
                    {
                        std::string addr((char *)&backendList->s.sin6_addr,16);
                        if(backendList->s.sin6_port == htobe16(80))
                            addressToHttp.erase(addr);
                        else
                            addressToHttps.erase(addr);
                    }
                    backendList=nullptr;
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                    break;
                }
                index++;
            }
            index=0;
            if(backendList!=nullptr)
                while(index<backendList->idle.size())
                {
                    if(backendList->idle.at(index)==this)
                    {
                        backendList->idle.erase(backendList->idle.cbegin()+index);
                        break;
                    }
                    index++;
                }
        }
    }
}

void Backend::downloadFinished()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " http " << http << " is finished, will be destruct" << std::endl;
    if(http==nullptr)
        std::cerr << __FILE__ << ":" << __LINE__ << "Backend::downloadFinished() http==nullptr bug suspected" << std::endl;
    #endif
    if(backendList==nullptr)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " http " << http << " backendList==nullptr return" << std::endl;
        #endif
        http=nullptr;
        return;
    }
    if(!wasTCPConnected)
    {
        size_t index=0;
        while(index<backendList->busy.size())
        {
            if(backendList->busy.at(index)==this)
            {
                backendList->busy.erase(backendList->busy.cbegin()+index);
                break;
            }
            index++;
        }
        if(!backendList->pending.empty() && backendList->busy.empty())
        {
            const std::string error("Tcp connect problem");
            size_t index=0;
            while(index<backendList->pending.size())
            {
                Http *http=backendList->pending.at(index);
                http->backendError(error);
                http->disconnectFrontend();
                //http->disconnectBackend();-> no backend because pending
                index++;
            }
        }
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
        #ifdef DEBUGFASTCGI
        std::cerr << "Backend::downloadFinished() NOT TRY AGAIN" << std::endl;
        #endif
        http->backend=nullptr;
        http=nullptr;
        return;
    }
    if(backendList->pending.empty())
    {
        size_t index=0;
        while(index<backendList->busy.size())
        {
            if(backendList->busy.at(index)==this)
            {
                backendList->busy.erase(backendList->busy.cbegin()+index);
                break;
            }
            index++;
        }
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
        if(this->isValid())
            backendList->idle.push_back(this);
        #ifdef DEBUGFASTCGI
        std::cerr << this << " backend, " << http << ": http->backend=null + http=nullptr" << " " << __FILE__ << ":" << __LINE__ << " isValid: " << this->isValid() << std::endl;
        #endif
        http->backend=nullptr;
        http=nullptr;
    }
    else
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
        std::cerr << http << ": http->backend=null and !backendList->pending.empty()" << std::endl;
        #endif
        http->backend=nullptr;
        http=nullptr;
        bool haveFound=false;
        bool haveUrlAndFrontendConnected=false;
        do
        {
            Http * httpToGet=backendList->pending.front();
            backendList->pending.erase(backendList->pending.cbegin());
            haveUrlAndFrontendConnected=httpToGet->haveUrlAndFrontendConnected();
            if(haveUrlAndFrontendConnected)
            {
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << ", link backend: " << this << " with http " << httpToGet << std::endl;
                #endif
                http=httpToGet;
                http->backend=this;
                http->readyToWrite();
                haveFound=true;
            }
            else
            {
                httpToGet->backendError("Internal error, !haveUrlAndFrontendConnected");
                httpToGet->disconnectFrontend();
                httpToGet->disconnectBackend();
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << ", http buggy, skipped: " << httpToGet << std::endl;
                #endif
            }
        } while(haveUrlAndFrontendConnected==false && !backendList->pending.empty());
        if(!haveFound)
        {
            size_t index=0;
            while(index<backendList->busy.size())
            {
                if(backendList->busy.at(index)==this)
                {
                    backendList->busy.erase(backendList->busy.cbegin()+index);
                    break;
                }
                index++;
            }
            #ifdef DEBUGFASTCGI
            std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " " << " isValid: " << this->isValid() << std::endl;
            #endif
            if(this->isValid())
                backendList->idle.push_back(this);
        }
        #ifdef DEBUGFASTCGI
        if(haveFound)
            std::cerr << __FILE__ << ":" << __LINE__ << ", found pending to do" << std::endl;
        else
            std::cerr << __FILE__ << ":" << __LINE__ << ", NO pending to do" << std::endl;
        #endif

    }
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
}

Backend * Backend::tryConnectInternalList(const sockaddr_in6 &s,Http *http,std::unordered_map<std::string,BackendList *> &addressToList,bool &connectInternal)
{
    connectInternal=true;
    std::string addr((char *)&s.sin6_addr,16);
    //if have already connected backend on this ip
    if(addressToList.find(addr)!=addressToList.cend())
    {
        BackendList *list=addressToList[addr];
        if(!list->idle.empty())
        {
            //assign to idle backend and become busy
            Backend *backend=list->idle.back();
            list->idle.pop_back();
            list->busy.push_back(backend);
            backend->http=http;
            #ifdef DEBUGFASTCGI
            std::cerr << http << ": http->backend=" << backend << " " << __FILE__ << ":" << __LINE__ << " isValid: " << backend->isValid() << std::endl;
            #endif
            http->backend=backend;
            http->readyToWrite();
            return backend;
        }
        else
        {
            if(list->busy.size()<Backend::maxBackend)
            {
                Backend *newBackend=new Backend(list);
                if(!newBackend->tryConnectInternal(s))
                {
                    connectInternal=false;
                    return nullptr;
                }
                newBackend->http=http;
                #ifdef DEBUGFASTCGI
                std::cerr << http << ": http->backend=" << newBackend << " " << __FILE__ << ":" << __LINE__ << " isValid: " << newBackend->isValid() << std::endl;
                #endif
                http->backend=newBackend;

                list->busy.push_back(newBackend);
                return newBackend;
            }
            else
            {
                list->pending.push_back(http);
                //list busy
                std::cerr << "backend busy on: ";
                int index=0;
                for(const Backend *b : list->busy)
                {
                    if(index>0)
                        std::cerr << ", ";
                    if(b==nullptr)
                        std::cerr << "no backend";
                    else if(b->http==nullptr)
                        std::cerr << "no http";
                    else
                        std::cerr << "url: " << b->http->getUrl();
                    index++;
                }
                std::cerr << std::endl;
                return nullptr;
            }
        }
    }
    else
    {
        BackendList *list=new BackendList();
        memcpy(&list->s,&s,sizeof(sockaddr_in6));

        Backend *newBackend=new Backend(list);
        if(!newBackend->tryConnectInternal(s))
        {
            connectInternal=false;
            return nullptr;
        }
        newBackend->http=http;
        #ifdef DEBUGFASTCGI
        std::cerr << http << ": http->backend=" << newBackend << " " << __FILE__ << ":" << __LINE__ << " isValid: " << newBackend->isValid() << std::endl;
        #endif
        http->backend=newBackend;

        list->busy.push_back(newBackend);
        addressToList[addr]=list;
        return newBackend;
    }
    #ifdef DEBUGFASTCGI
    std::cerr << http << ": http->backend out of condition" << std::endl;
    #endif
    return nullptr;
}

Backend * Backend::tryConnectHttp(const sockaddr_in6 &s,Http *http, bool &connectInternal)
{
    return tryConnectInternalList(s,http,addressToHttp,connectInternal);
}

void Backend::startHttps()
{
    if(ssl!=nullptr)
    {
        std::cerr << "Backend::startHttps(): ssl!=nullptr at start" << std::endl;
        return;
    }
    #ifdef DEBUGFASTCGI
    std::cerr << "Backend::startHttps(): " << this << std::endl;
    #endif

    /* ------------------------------------- */
    meth = TLS_client_method();
    ctx = SSL_CTX_new(meth);
    if (ctx==nullptr)
    {
        std::cerr << "ctx = SSL_CTX_new(meth); return NULL" << std::endl;
        return;
    }

    /* ---------------------------------------------------------------- */
    /* Cipher AES128-GCM-SHA256 and AES256-GCM-SHA384 - good performance with AES-NI support. */
    if (!SSL_CTX_set_cipher_list(ctx, "AES128-GCM-SHA256")) {
        printf("Could not set cipher list");
        return;
    }
    /* ------------------------------- */
    /* Configure certificates and keys */
    if (!SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION)) {
        printf("Could not disable compression");
        return;
    }
/*    if (SSL_CTX_load_verify_locations(ctx, CERTF, 0) <= 0) {
        ERR_print_errors_fp(stderr);
        return;
    }
    if (SSL_CTX_use_certificate_file(ctx, CERTF, SSL_FILETYPE_PEM) <= 0) {
        printf("Could not load cert file: ");
        ERR_print_errors_fp(stderr);
        return;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, KEYF, SSL_FILETYPE_PEM) <= 0) {
        printf("Could not load key file");
        ERR_print_errors_fp(stderr);
        return;
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        fprintf(stderr,
                "Private key does not match public key in certificate.\n");
        return;
    }*/
    /* Enable client certificate verification. Enable before accepting connections. */
    /*SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT |
    SSL_VERIFY_CLIENT_ONCE, 0);*/
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, 0);

    /* Start SSL negotiation, connection available. */
    ssl = SSL_new(ctx);
    if (ssl==nullptr)
    {
        std::cerr << "SSL_new(ctx); return NULL" << std::endl;
        return;
    }

    if(!SSL_set_fd(ssl, fd))
    {
        printf("SSL_set_fd failed");
        closeSSL();
        #if defined(DEBUGFILEOPEN) || defined(MAXFILESIZE)
        std::cerr << "Backend::startHttps(), fd: " << fd << ", err == SSL_ERROR_ZERO_RETURN" << std::endl;
        #endif
        Cache::closeFD(fd);
        ::close(fd);
        fd=-1;
        return;
    }
    SSL_set_connect_state(ssl);

    for(;;) {
        int success = SSL_connect(ssl);

        if(success < 0) {
            int err = SSL_get_error(ssl, success);

            /* Non-blocking operation did not complete. Try again later. */
            if (err == SSL_ERROR_WANT_READ ||
                    err == SSL_ERROR_WANT_WRITE ||
                    err == SSL_ERROR_WANT_X509_LOOKUP) {
                continue;
            }
            else if(err == SSL_ERROR_ZERO_RETURN) {
                printf("SSL_connect: close notify received from peer");
                closeSSL();
                #if defined(DEBUGFILEOPEN) || defined(MAXFILESIZE)
                std::cerr << "Backend::startHttps(), fd: " << fd << ", err == SSL_ERROR_ZERO_RETURN" << std::endl;
                #endif
                Cache::closeFD(fd);
                ::close(fd);
                fd=-1;
                return;
            }
            else {
                printf("Error SSL_connect: %d", err);
                perror("perror: ");
                closeSSL();
                #if defined(DEBUGFILEOPEN) || defined(MAXFILESIZE)
                std::cerr << "Backend::startHttps(), fd: " << fd << std::endl;
                #endif
                Cache::closeFD(fd);
                ::close(fd);
                fd=-1;
                return;
            }
        }
        else {
            #ifdef DEBUGHTTPS
            dump_cert_info(ssl, false);
            #else
            #ifdef DEBUGFASTCGI
            std::cerr << "problem with certificate" << std::endl;
            #endif
            #endif
            break;
        }
    }
}

Backend * Backend::tryConnectHttps(const sockaddr_in6 &s,Http *http, bool &connectInternal)
{
    return tryConnectInternalList(s,http,addressToHttps,connectInternal);
}

#ifdef DEBUGHTTPS
void Backend::dump_cert_info(SSL *ssl, bool server)
{
    if(server) {
        printf("Ssl server version: %s", SSL_get_version(ssl));
    }
    else {
        printf("Client Version: %s", SSL_get_version(ssl));
    }

    /* The cipher negotiated and being used */
    printf("Using cipher %s", SSL_get_cipher(ssl));

    /* Get client's certificate (note: beware of dynamic allocation) - opt */
    X509 *client_cert = SSL_get_peer_certificate(ssl);
    if (client_cert != NULL) {
        if(server) {
        printf("Client certificate:\n");
        }
        else {
            printf("Server certificate:\n");
        }
        char *str = X509_NAME_oneline(X509_get_subject_name(client_cert), 0, 0);
        if(str == NULL) {
            printf("warn X509 subject name is null");
        }
        printf("\t Subject: %s\n", str);
        OPENSSL_free(str);

        str = X509_NAME_oneline(X509_get_issuer_name(client_cert), 0, 0);
        if(str == NULL) {
            printf("warn X509 issuer name is null");
        }
        printf("\t Issuer: %s\n", str);
        OPENSSL_free(str);

        /* Deallocate certificate, free memory */
        X509_free(client_cert);
    } else {
        printf("Client does not have certificate.\n");
    }
}
#endif

bool Backend::tryConnectInternal(const sockaddr_in6 &s)
{
    /* --------------------------------------------- */
    /* Create a normal socket and connect to server. */

    fd = socket(AF_INET6, SOCK_STREAM, 0);
    if(fd==-1)
    {
        std::cerr << "Unable to create socket" << std::endl;
        return false;
    }
    Cache::newFD(fd,this,EpollObject::Kind::Kind_Backend);

    char astring[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &(s.sin6_addr), astring, INET6_ADDRSTRLEN);
    #ifdef DEBUGFASTCGI
    if(std::string(astring)=="::")
    {
        std::cerr << "Internal error, try connect on ::" << std::endl;
        return false;
    }
    printf("Try connect on %s %i\n", astring, be16toh(s.sin6_port));
    std::cerr << std::endl;
    std::cout << std::endl;
    #endif
    https=(s.sin6_port==https_portBE);

    // non-blocking client socket
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) {
            std::cerr << "fcntl(fd, F_GETFL, 0); return < 0" << std::endl;
            //return false;
        }
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    // no delay
    {
        int flag = 1;
        setsockopt(fd,            /* socket affected */
        IPPROTO_TCP,     /* set option at TCP level */
        TCP_NODELAY,     /* name of option */
        (char *) &flag,  /* the cast is historical
        cruft */
        sizeof(int));
    }

    // ---------------------

    struct epoll_event event;
    event.data.ptr = this;
    event.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLERR | EPOLLHUP | EPOLLRDHUP;

    int t = epoll_ctl(EpollObject::epollfd, EPOLL_CTL_ADD, fd, &event);
    if (t == -1) {
        std::cerr << "epoll_ctl(EpollObject::epollfd, EPOLL_CTL_ADD, fd, &event); return -1" << std::endl;
        Cache::closeFD(fd);
        ::close(fd);
        fd=-1;
        return false;
    }

    /*sockaddr_in6 targetDnsIPv6;
    targetDnsIPv6.sin6_port = htobe16(53);
    const char * const hostC=host.c_str();
    int convertResult=inet_pton(AF_INET6,hostC,&targetDnsIPv6.sin6_addr);*/
    int err = connect(fd, (struct sockaddr*) &s, sizeof(s));
    if (err < 0 && errno != EINPROGRESS)
    {
        std::cerr << "connect != EINPROGRESS" << std::endl;
        return false;
    }
    return true;
}

void Backend::parseEvent(const epoll_event &event)
{
    #ifdef DEBUGFASTCGI
    if(event.events & ~EPOLLOUT & ~EPOLLIN)
        std::cout << this << " Backend::parseEvent event.events: " << event.events << std::endl;
    #endif
    if(event.events & EPOLLIN)
    {
        #ifdef DEBUGFASTCGI
        //std::cout << "EPOLLIN" << std::endl;
        #endif
        if(http!=nullptr)
            http->readyToRead();
        else
        {
            std::cerr << "Received data while not connected to http" << std::endl;
            char buffer[1024*1024];
            while(Backend::socketRead(buffer,sizeof(buffer))>0)
            {}
        }
    }
    if(event.events & EPOLLOUT)
    {
        /*#ifdef DEBUGFASTCGI
        std::cout << "EPOLLOUT" << std::endl;
        #endif*/
        if(ssl==nullptr && https)
            startHttps();
        if(http!=nullptr)
            http->readyToWrite();
    }

    if(event.events & EPOLLHUP)
    {
        #ifdef DEBUGFASTCGI
        std::cout << "EPOLLHUP" << std::endl;
        #endif
        remoteSocketClosed();
        //do client reject
    }
    if(event.events & EPOLLRDHUP)
    {
        #ifdef DEBUGFASTCGI
        std::cout << "EPOLLRDHUP" << std::endl;
        #endif
        remoteSocketClosed();
    }
    if(event.events & EPOLLET)
    {
        #ifdef DEBUGFASTCGI
        std::cout << "EPOLLET" << std::endl;
        #endif
        remoteSocketClosed();
    }
    if(event.events & EPOLLERR)
    {
        #ifdef DEBUGFASTCGI
        std::cout << "EPOLLERR" << std::endl;
        #endif
        remoteSocketClosed();
    }
}

void Backend::readyToWrite()
{
    if(bufferSocket.empty())
        return;
    const ssize_t &sizeW=::write(fd,bufferSocket.data(),bufferSocket.size());
    if(sizeW>=0)
    {
        if((size_t)sizeW<bufferSocket.size())
            this->bufferSocket.erase(0,bufferSocket.size()-sizeW);
        else
            this->bufferSocket.clear();
    }
}

ssize_t Backend::socketRead(void *buffer, size_t size)
{
    #ifdef DEBUGFASTCGI
    //std::cout << "Socket try read" << std::endl;
    if(http==nullptr)
    {
        std::cerr << this << " " << "socketRead() when no http set" << std::endl;
        errno=0;
        return -1;
    }
    #endif
    if(fd<0)
    {
        errno=0;
        return -1;
    }
    if(ssl!=nullptr)
    {
        int readen = SSL_read(ssl, buffer, size);
        if (readen<0)
        {
            if(errno!=11)
                std::cerr << this << " " << "SSL_read return -1 with errno " << errno << std::endl;
            return -1;
        }
        #ifdef MAXFILESIZE
        byteReceived+=readen;
        if(byteReceived>100000000)
        {
            std::cerr << this << " " << "Byte received from SSL is > 100MB (abort)" << std::endl;
            abort();
        }
        #endif
        /*#ifdef DEBUGFASTCGI
        std::cout << "Socket byte read: " << readen << std::endl;
        std::cerr << "Client Received " << readen << " chars - '" << std::string((char *)buffer,readen) << "'" << std::endl;
        #endif*/

        if (readen <= 0) {
            if(readen == SSL_ERROR_WANT_READ ||
                readen == SSL_ERROR_WANT_WRITE ||
                readen == SSL_ERROR_WANT_X509_LOOKUP) {
                std::cerr << this << " " << "Read could not complete. Will be invoked later." << std::endl;
                return -1;
            }
            else if(readen == SSL_ERROR_ZERO_RETURN) {
                std::cerr << this << " " << "SSL_read: close notify received from peer" << std::endl;
                return -1;
            }
            else {
                std::cerr << this << " " << "Error during SSL_read" << std::endl;
                return -1;
            }
            std::cerr << this << " " << "Error during SSL_read bis" << std::endl;
            return -1;
        }
        else
        {
            lastReceivedBytesTimestamps=Backend::currentTime();
            return readen;
        }
    }
    else
    {
        const ssize_t &s=::read(fd,buffer,size);
        #ifdef MAXFILESIZE
        byteReceived+=s;
        if(byteReceived>100000000)
        {
            std::cerr << "Byte received from SSL is > 100MB (abort)" << std::endl;
            abort();
        }
        #endif
        /*#ifdef DEBUGFASTCGI
        std::cout << "Socket byte read: " << s << std::endl;
        #endif*/
        if(s>0)
            lastReceivedBytesTimestamps=Backend::currentTime();
        return s;
    }
}

bool Backend::socketWrite(const void *buffer, size_t size)
{
    #ifdef MAXFILESIZE
    fileGetCount++;
    if(fileGetCount>500)
    {
        std::cerr << this << " " << "socketRead() fileGetCount>500" << std::endl;
        close();
        errno=0;
        return false;
    }
    #endif
    #ifdef DEBUGFASTCGI
    std::cout << this << " " << "Try socket write: " << size << std::endl;
    if(http==nullptr)
    {
        std::cerr << this << " " << "socketRead() when no http set" << std::endl;
        errno=0;
        return false;
    }
    #endif
    if(fd<0)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << "Backend::socketWrite() fd<0" << std::endl;
        #endif
        return false;
    }
    if(!this->bufferSocket.empty())
    {
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << "Backend::socketWrite() !this->bufferSocket.empty()" << std::endl;
        #endif
        this->bufferSocket+=std::string((char *)buffer,size);
        return true;
    }
    #ifdef MAXFILESIZE
    {
        struct stat sb;
        int rstat=fstat(fd,&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << this << " " << "too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << std::endl;
            abort();
        }
    }
    #endif
    ssize_t sizeW=-1;
    if(ssl!=nullptr)
    {
        #ifdef DEBUGFASTCGI
        std::cout << this << " " << "Try SSL socket write: " << size << std::endl;
        #endif
        #ifdef DEBUGFASTCGI
        //std::cerr << "Client Send " << size << " chars - '" << std::string((char *)buffer,size) << "'" << std::endl;
        #endif
        int writenSize = SSL_write(ssl, buffer,size);
        if (writenSize <= 0) {
            if(writenSize == SSL_ERROR_WANT_READ ||
                writenSize == SSL_ERROR_WANT_WRITE ||
                writenSize == SSL_ERROR_WANT_X509_LOOKUP) {
                std::cerr << this << " SSL_write(ssl, buffer,size); return -1 Write could not complete. Will be invoked later., errno " << errno << " fd: " << getFD() << std::endl;
                return false;
            }
            else if(writenSize == SSL_ERROR_ZERO_RETURN) {
                std::cerr << this << " SSL_write(ssl, buffer,size); return -1 close notify received from peer, errno " << errno << " fd: " << getFD() << std::endl;
                return false;
            }
            else {
                std::cerr << this << " SSL_write(ssl, buffer,size); return -1, errno " << errno << " fd: " << getFD() << std::endl;
                return false;
            }
        }
        else
            sizeW=writenSize;
    }
    else
    {
        #ifdef DEBUGFASTCGI
        std::cout << this << " " << "Try socket write: " << size << std::endl;
        #endif
        sizeW=::write(fd,buffer,size);
    }
    #ifdef MAXFILESIZE
    {
        struct stat sb;
        int rstat=fstat(fd,&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << this << " " << "too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << std::endl;
            abort();
        }
    }
    #endif
    #ifdef DEBUGFASTCGI
    std::cout << "Socket Writed bytes: " << size << std::endl;
    #endif
    if(sizeW>=0)
    {
        if((size_t)sizeW<size)
        {
            #ifdef DEBUGFASTCGI
            std::cerr << this << " " << "sizeW only: " << sizeW << std::endl;
            #endif
            this->bufferSocket+=std::string((char *)buffer+sizeW,size-sizeW);
        }
        return true;
    }
    else
    {
        if(errno!=32)//if not broken pipe
            std::cerr << this << " " << "Http socket errno:" << errno << std::endl;
        return false;
    }
}

uint64_t Backend::currentTime() //ms from 1970
{
    struct timeval te;
    gettimeofday(&te, NULL);
    return te.tv_sec*1000LL + te.tv_usec/1000;
}

bool Backend::detectTimeout()
{
    //if no http then idle, no data, skip detect timeout
    if(http==nullptr)
        return false;
    const uint64_t msFrom1970=Backend::currentTime();
    if(lastReceivedBytesTimestamps>(msFrom1970-5*1000))
    {
        //prevent time drift
        if(lastReceivedBytesTimestamps>msFrom1970)
            lastReceivedBytesTimestamps=msFrom1970;
        return false;
    }
    //if no byte received into 5s
    #ifdef DEBUGFASTCGI
    struct timeval tv;
    gettimeofday(&tv,NULL);
    std::cerr << "[" << tv.tv_sec << "] ";
    #endif
    std::cerr << "Backend::detectTimeout() timeout while downloading " << http->getUrl() << " from " << http << " (backend " << this << ")" << std::endl;
    if(http!=nullptr)
        http->backendError("Timeout");
    if(http!=nullptr)
        http->disconnectFrontend();
    if(http!=nullptr)
        http->disconnectBackend();
    close();
    //abort();
    return true;
}

std::string Backend::getQuery() const
{
    std::string ret;
    if(http==nullptr)
        ret+="not alive";
    else
        ret+="alive on "+http->getUrl();
    ret+=" last byte "+std::to_string(lastReceivedBytesTimestamps);
    if(wasTCPConnected)
        ret+=" wasTCPConnected";
    else
        ret+=" !wasTCPConnected";
    return ret;
}
