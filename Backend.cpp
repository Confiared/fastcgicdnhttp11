#include "Backend.hpp"
#include "Http.hpp"
#include <iostream>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/tcp.h>

//curl -v -H "Accept-Encoding: gzip" -o style.css.gz 'http://cdn.bolivia-online.com/ultracopier-static.first-world.info/css/style.css'

std::unordered_map<std::string,Backend::BackendList *> Backend::addressToHttp;
std::unordered_map<std::string,Backend::BackendList *> Backend::addressToHttps;

static uint16_t https_portBE=be16toh(443);

Backend::Backend(BackendList * backendList) :
    http(nullptr),
    https(false),
    wasTCPConnected(false),
    backendList(backendList),
    meth(nullptr),
    ctx(nullptr),
    ssl(nullptr)
{
    this->kind=EpollObject::Kind::Kind_Backend;
}

Backend::~Backend()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    if(fd!=-1)
    {
        std::cerr << "EPOLL_CTL_DEL Http: " << fd << std::endl;
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
}

void Backend::remoteSocketClosed()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    if(fd!=-1)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << "EPOLL_CTL_DEL remoteSocketClosed Http: " << fd << std::endl;
        #endif
        if(epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL)==-1)
            std::cerr << "EPOLL_CTL_DEL remoteSocketClosed Http: " << fd << ", errno: " << errno << std::endl;
        ::close(fd);
        fd=-1;
    }
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
                    index++;
                }
            }
            if(http!=nullptr)
                http->backend=nullptr;
            #ifdef DEBUGFASTCGI
            std::cerr << "remoteSocketClosed " << __FILE__ << ":" << __LINE__ << std::endl;
            #endif
            return;
        }
        else
        {
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
            #endif
            size_t index=0;
            while(index<backendList->busy.size())
            {
                if(backendList->busy.at(index)==this)
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                    backendList->busy.erase(backendList->busy.cbegin()+index);
                    if(http!=nullptr)
                    {
                        /*if(http->requestSended)
                        {
                            std::cerr << "reassign but request already send" << std::endl;
                            http->parseNonHttpError(Backend::NonHttpError_AlreadySend);
                            return;
                        }*/
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
                            std::cerr << http << ": http->backend=" << backend << std::endl;
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
                            std::cerr << http << ": http->backend=" << newBackend << std::endl;
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
    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
    if(http==nullptr)
        std::cerr << __FILE__ << ":" << __LINE__ << "Backend::downloadFinished() http==nullptr bug suspected" << std::endl;
    #endif
    if(backendList==nullptr)
        return;
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
        backendList->idle.push_back(this);
        #ifdef DEBUGFASTCGI
        std::cerr << this << " backend, " << http << ": http->backend=null + http=nullptr" << std::endl;
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
            std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
            #endif
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
            std::cerr << http << ": http->backend=" << backend << std::endl;
            #endif
            http->backend=backend;
            http->readyToWrite();
            return backend;
        }
        else
        {
            if(list->busy.size()<MAXBACKEND)
            {
                Backend *newBackend=new Backend(list);
                if(!newBackend->tryConnectInternal(s))
                {
                    connectInternal=false;
                    return nullptr;
                }
                newBackend->http=http;
                #ifdef DEBUGFASTCGI
                std::cerr << http << ": http->backend=" << newBackend << std::endl;
                #endif
                http->backend=newBackend;

                list->busy.push_back(newBackend);
                return newBackend;
            }
            else
            {
                list->pending.push_back(http);
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
        std::cerr << http << ": http->backend=" << newBackend << std::endl;
        #endif
        http->backend=newBackend;

        list->busy.push_back(newBackend);
        addressToList[addr]=list;
        return newBackend;
    }
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
        abort();
    }
    #ifdef DEBUGFASTCGI
    std::cerr << "Backend::startHttps(): " << this << std::endl;
    #endif
    /* ------------ */
    /* Init openssl */
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    /* ------------------------------------- */
    meth = TLS_client_method();
    ctx = SSL_CTX_new(meth);
    if (ctx==NULL)
    {
        std::cerr << "ctx = SSL_CTX_new(meth); return NULL" << std::endl;
        abort();
    }

    /* ---------------------------------------------------------------- */
    /* Cipher AES128-GCM-SHA256 and AES256-GCM-SHA384 - good performance with AES-NI support. */
    if (!SSL_CTX_set_cipher_list(ctx, "AES128-GCM-SHA256")) {
        printf("Could not set cipher list");
        abort();
    }
    /* ------------------------------- */
    /* Configure certificates and keys */
    if (!SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION)) {
        printf("Could not disable compression");
        abort();
    }
/*    if (SSL_CTX_load_verify_locations(ctx, CERTF, 0) <= 0) {
        ERR_print_errors_fp(stderr);
        abort();
    }
    if (SSL_CTX_use_certificate_file(ctx, CERTF, SSL_FILETYPE_PEM) <= 0) {
        printf("Could not load cert file: ");
        ERR_print_errors_fp(stderr);
        abort();
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, KEYF, SSL_FILETYPE_PEM) <= 0) {
        printf("Could not load key file");
        ERR_print_errors_fp(stderr);
        abort();
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        fprintf(stderr,
                "Private key does not match public key in certificate.\n");
        abort();
    }*/
    /* Enable client certificate verification. Enable before accepting connections. */
    /*SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT |
    SSL_VERIFY_CLIENT_ONCE, 0);*/
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, 0);

    /* Start SSL negotiation, connection available. */
    ssl = SSL_new(ctx);
    if (ssl==NULL)
    {
        std::cerr << "SSL_new(ctx); return NULL" << std::endl;
        abort();
    }

    SSL_set_fd(ssl, fd);
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
                abort();
            }
            else {
                printf("Error SSL_connect: %d", err);
                perror("perror: ");
                SSL_free(ssl);
                meth=nullptr;
                ctx=nullptr;
                ssl=nullptr;
                close(fd);
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

    char astring[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &(s.sin6_addr), astring, INET6_ADDRSTRLEN);
    #ifdef DEBUGFASTCGI
    if(std::string(astring)=="::")
    {
        std::cerr << "Internal error, try connect on ::" << std::endl;
        abort();
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
            abort();
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
        abort();
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
    if(event.events & EPOLLIN)
    {
        #ifdef DEBUGFASTCGI
        std::cout << "EPOLLIN" << std::endl;
        #endif
        if(http!=nullptr)
            http->readyToRead();
    }
    if(event.events & EPOLLOUT)
    {
        #ifdef DEBUGFASTCGI
        std::cout << "EPOLLOUT" << std::endl;
        #endif
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
    std::cout << "Socket try read" << std::endl;
    if(http==nullptr)
    {
        std::cerr << "socketRead() when no http set" << std::endl;
        abort();
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
        if (readen==-1)
        {
            std::cerr << "SSL_read return -1" << std::endl;
            return -1;
        }
        #ifdef DEBUGFASTCGI
        std::cout << "Socket byte read: " << readen << std::endl;
        std::cerr << "Client Received " << readen << " chars - '" << std::string((char *)buffer,readen) << "'" << std::endl;
        #endif

        if (readen <= 0) {
            if(readen == SSL_ERROR_WANT_READ ||
                readen == SSL_ERROR_WANT_WRITE ||
                readen == SSL_ERROR_WANT_X509_LOOKUP) {
                printf("Read could not complete. Will be invoked later.");
                return -1;
            }
            else if(readen == SSL_ERROR_ZERO_RETURN) {
                printf("SSL_read: close notify received from peer");
                return -1;
            }
            else {
                printf("Error during SSL_read");
                return -1;
            }
            return -1;
        }
        else
            return readen;
    }
    else
    {
        const ssize_t &s=::read(fd,buffer,size);
        #ifdef DEBUGFASTCGI
        std::cout << "Socket byte read: " << s << std::endl;
        #endif
        return s;
    }
}

bool Backend::socketWrite(const void *buffer, size_t size)
{
    #ifdef DEBUGFASTCGI
    std::cout << "Try socket write: " << size << std::endl;
    if(http==nullptr)
    {
        std::cerr << "socketRead() when no http set" << std::endl;
        abort();
    }
    #endif
    if(fd<0)
        return false;
    if(!this->bufferSocket.empty())
    {
        this->bufferSocket+=std::string((char *)buffer,size);
        return true;
    }
    ssize_t sizeW=-1;
    if(ssl!=nullptr)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << "Client Send " << size << " chars - '" << std::string((char *)buffer,size) << "'" << std::endl;
        #endif
        int writenSize = SSL_write(ssl, buffer,size);
        if (writenSize==-1)
        {
            std::cerr << "SSL_write(ssl, buffer,size); return -1" << std::endl;
            abort();
        }

        if (writenSize <= 0) {
            if(writenSize == SSL_ERROR_WANT_READ ||
                writenSize == SSL_ERROR_WANT_WRITE ||
                writenSize == SSL_ERROR_WANT_X509_LOOKUP) {
                printf("Write could not complete. Will be invoked later.");
                return false;
            }
            else if(writenSize == SSL_ERROR_ZERO_RETURN) {
                printf("SSL_write: close notify received from peer");
                return false;
            }
            else {
                printf("Error during SSL_write");
                return false;
            }
        }
        else
            sizeW=writenSize;
    }
    else
        sizeW=::write(fd,buffer,size);
    #ifdef DEBUGFASTCGI
    std::cout << "Socket Writed bytes: " << size << std::endl;
    #endif
    if(sizeW>=0)
    {
        if((size_t)sizeW<size)
        {
            #ifdef DEBUGFASTCGI
            std::cerr << "sizeW only: " << sizeW << std::endl;
            #endif
            this->bufferSocket+=std::string((char *)buffer+sizeW,size-sizeW);
        }
        return true;
    }
    else
    {
        if(errno!=32)//if not broken pipe
            std::cerr << "Http socket errno:" << errno << std::endl;
        return false;
    }
}
