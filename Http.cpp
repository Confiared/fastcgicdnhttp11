#include "Http.hpp"
#ifdef DEBUGFASTCGI
#include "Https.hpp"
#endif
#include "Client.hpp"
#include "Cache.hpp"
#include "Backend.hpp"
#include "Common.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <sstream>
#include <algorithm>
#include <fcntl.h>
#include <chrono>

#ifdef DEBUGFASTCGI
#include <arpa/inet.h>
#include <sys/time.h>
#endif

#ifdef DEBUGFASTCGI
std::unordered_set<Http *> Http::toDebug;
#endif
std::unordered_set<Http *> Http::toDelete;

//ETag -> If-None-Match
const char rChar[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
const size_t &rCharSize=sizeof(rChar)-1;
//Todo: limit max file size 9GB
//reuse cache stale for file <20KB

std::unordered_map<std::string,Http *> Http::pathToHttp;
int Http::fdRandom=-1;
char Http::buffer[1024*1024];

Http::Http(const int &cachefd, //0 if no old cache file found
           const std::string &cachePath) :
    cachePath(cachePath),//to remove from Http::pathToHttp
    tempCache(nullptr),
    finalCache(nullptr),
    parsedHeader(false),
    lastReceivedBytesTimestamps(0),
    contentsize(-1),
    contentwritten(0),
    http_code(0),
    parsing(Parsing_None),
    pending(false),
    requestSended(false),
    headerWriten(false),
    backend(nullptr),
    contentLengthPos(-1),
    chunkLength(-1)
{
    #ifdef DEBUGFASTCGI
    toDebug.insert(this);
    #endif
    endDetected=false;
    lastReceivedBytesTimestamps=Backend::currentTime();
    #ifdef DEBUGFASTCGI
    if(&pathToHttpList()==&Http::pathToHttp)
        std::cerr << "contructor http " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
    else
        std::cerr << "contructor https " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
    if(cachePath.empty())
    {
        std::cerr << "critical error cachePath.empty() " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
        abort();
    }
    #endif
    if(cachefd<=0)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << "Http::Http() cachefd==0 then tempCache(nullptr): " << this << std::endl;
        #endif
    }
    else
    {
        #ifdef DEBUGFASTCGI
        std::cerr << "Http::Http() cachefd!=0: " << this << std::endl;
        #endif
        finalCache=new Cache(cachefd);
    }
    /*
    //while receive write to cache
    //when finish
        //unset Http to all future listener
        //Close all listener
    */
}

/// \bug never call! memory leak
Http::~Http()
{
    #ifdef DEBUGFASTCGI
    if(toDebug.find(this)!=toDebug.cend())
        toDebug.erase(this);
    else
    {
        std::cerr << "Http Entry not found into global list, abort()" << std::endl;
        abort();
    }
    #endif
    #ifdef DEBUGFASTCGI
    std::cerr << "Http::~Http(): destructor " << this << "uri: " << uri<< ": " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    Backend *b=backend;
    delete tempCache;
    tempCache=nullptr;
    disconnectFrontend();
    disconnectBackend(true);
    for(Client * client : clientsList)
    {
        client->writeEnd();
        client->disconnect();
    }
    clientsList.clear();

    #ifdef DEBUGFASTCGI
    for(const Client * client : Client::clients)
    {
        if(client->http==this)
        {
            std::cerr << "Http::~Http(): destructor, remain client on this http " << __FILE__ << ":" << __LINE__ << std::endl;
            abort();
        }
    }
    {
        std::unordered_map<std::string/* example: cache/29E7336BDEA3327B */,Http *> pathToHttp=Http::pathToHttp;
        for( const auto &n : pathToHttp )
            if(n.second==this)
            {
                std::cerr << "Http::~Http(): destructor post opt this " << this << " can't be into Http::pathToHttp at " << n.first << " " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
    }
    {
        std::unordered_map<std::string/* example: cache/29E7336BDEA3327B */,Http *> pathToHttp=Https::pathToHttps;
        for( const auto &n : pathToHttp )
            if(n.second==this)
            {
                std::cerr << "Http::~Http(): destructor post opt this " << this << " can't be into Https::pathToHttps at " << n.first << " " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
    }
    if(Http::toDelete.find(this)!=Http::toDelete.cend())
    {
        std::cerr << "Http::~Http(): destructor post opt can't have this into Http::toDelete " << this << __FILE__ << ":" << __LINE__ << std::endl;
        abort();
    }
    if(b!=nullptr)
    {
        if(b->http==this)
        {
            std::cerr << "Http::~Http(): destructor post backend " << (void *)b << " remain on this Http " << this << __FILE__ << ":" << __LINE__ << std::endl;
            abort();
        }
    }
    #endif
}

bool Http::tryConnect(const sockaddr_in6 &s,const std::string &host,const std::string &uri,const std::string &etag)
{
    #ifdef DEBUGFASTCGI
    const auto p1 = std::chrono::system_clock::now();
    std::cerr << std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count() << " try connect " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
    if(etag.find('\0')!=std::string::npos)
        std::cerr << "etag contain \\0 abort" << __FILE__ << ":" << __LINE__ << std::endl;
    m_socket=s;
    #endif
    this->host=host;
    this->uri=uri;
    this->etagBackend=etag;
    return tryConnectInternal(s);
}

bool Http::tryConnectInternal(const sockaddr_in6 &s)
{
    bool connectInternal=false;
    backend=Backend::tryConnectHttp(s,this,connectInternal,&backendList);
    if(backend==nullptr)
    {
        std::string host2="Unknown IPv6";
        char str[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &m_socket.sin6_addr, str, INET6_ADDRSTRLEN) != NULL)
            host2=str;
        const auto p1 = std::chrono::system_clock::now();
        std::cerr << std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count() << " " << this << ": unable to get backend for " << host << uri << " Backend::addressToHttp[" << host2 << "]" << std::endl;
        #ifdef DEBUGFASTCGI
        //check here if not backend AND free backend or backend count < max
        std::string addr((char *)&m_socket.sin6_addr,16);
        //if have already connected backend on this ip
        if(Backend::addressToHttp.find(addr)!=Backend::addressToHttp.cend())
        {
            Backend::BackendList *list=Backend::addressToHttp[addr];
            if(!list->idle.empty())
            {
                std::cerr << this << " backend==nullptr and !list->idle.empty(), isAlive(): " << std::to_string((int)isAlive()) << ", clientsList size: " << std::to_string(clientsList.size()) << " (abort)" << std::endl;
                abort();
            }
            if(list->busy.size()<Backend::maxBackend)
            {
                std::cerr << this << " backend==nullptr and list->busy.size()<Backend::maxBackend, isAlive(): " << std::to_string((int)isAlive()) << ", clientsList size: " << std::to_string(clientsList.size()) << " (abort)" << std::endl;
                abort();
            }
            unsigned int index=0;
            while(index<list->pending.size())
            {
                if(list->pending.at(index)==this)
                    break;
                index++;
            }
            if(index>=list->pending.size())
            {
                std::cerr << this << " backend==nullptr and this " << this << " not found into pending, isAlive(): " << std::to_string((int)isAlive()) << ", clientsList size: " << std::to_string(clientsList.size()) << " (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
        }
        else if(Backend::addressToHttps.find(addr)!=Backend::addressToHttps.cend())
        {
            Backend::BackendList *list=Backend::addressToHttps[addr];
            if(!list->idle.empty())
            {
                std::cerr << this << " backend==nullptr and !list->idle.empty(), isAlive(): " << std::to_string((int)isAlive()) << ", clientsList size: " << std::to_string(clientsList.size()) << " (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
            if(list->busy.size()<Backend::maxBackend)
            {
                std::cerr << this << " backend==nullptr and list->busy.size()<Backend::maxBackend, isAlive(): " << std::to_string((int)isAlive()) << ", clientsList size: " << std::to_string(clientsList.size()) << " (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
            unsigned int index=0;
            while(index<list->pending.size())
            {
                if(list->pending.at(index)==this)
                    break;
                index++;
            }
            if(index>=list->pending.size())
            {
                std::cerr << this << " backend==nullptr and this " << this << " not found into pending, isAlive(): " << std::to_string((int)isAlive()) << ", clientsList size: " << std::to_string(clientsList.size()) << " (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
        }
        else
        {
            std::string host="Unknown IPv6";
            std::string host2="Unknown IPv6";
            char str[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, &m_socket.sin6_addr, str, INET6_ADDRSTRLEN) != NULL)
                host=str;
            if (inet_ntop(AF_INET6, &m_socket.sin6_addr, str, INET6_ADDRSTRLEN) != NULL)
                host2=str;
            std::cerr << this << " backend==nullptr and no backend list found, isAlive(): " << std::to_string((int)isAlive()) << ", clientsList size: " << std::to_string(clientsList.size()) << " " << host << " " << host2 << " (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
            abort();
        }
        #endif
    }
    #ifdef DEBUGFASTCGI
    std::cerr << this << ": http->backend=" << backend << std::endl;
    #endif
    return connectInternal && backend!=nullptr;
}

const std::string &Http::getCachePath() const
{
    return cachePath;
}

void Http::resetRequestSended()
{
    if(http_code!=0)
        return;
    parsedHeader=false;
    contentsize=-1;
    contentwritten=0;
    parsing=Parsing_None;
    requestSended=false;
    contentLengthPos=-1;
    chunkLength=-1;
}

void Http::sendRequest()
{
    //reset lastReceivedBytesTimestamps when come from busy to pending
    lastReceivedBytesTimestamps=Backend::currentTime();

    #ifdef DEBUGFASTCGI
    struct timeval tv;
    gettimeofday(&tv,NULL);
    std::cerr << "[" << tv.tv_sec << "] ";
    std::cerr << "Http::sendRequest() " << this << " " << __FILE__ << ":" << __LINE__ << " uri: " << uri << std::endl;
    if(uri.empty())
    {
        std::cerr << "Http::readyToWrite(): but uri.empty()" << std::endl;
        flushRead();
        return;
    }
    #endif
    requestSended=true;
    if(etagBackend.empty())
    {
        std::string h(std::string("GET ")+uri+" HTTP/1.1\r\nHost: "+host+"\r\nEPNOERFT: ysff43Uy\r\n"
              #ifdef HTTPGZIP
              "Accept-Encoding: gzip\r\n"+
              #endif
                      "\r\n");
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
        //std::cerr << h << std::endl;
        #endif
        if(!socketWrite(h.data(),h.size()))
            std::cerr << "ERROR to write: " << h << " errno: " << errno << std::endl;
    }
    else
    {
        std::string h(std::string("GET ")+uri+" HTTP/1.1\r\nHost: "+host+"\r\nEPNOERFT: ysff43Uy\r\nIf-None-Match: "+etagBackend+"\r\n"
              #ifdef HTTPGZIP
              "Accept-Encoding: gzip\r\n"+
              #endif
                      "\r\n");
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
        //std::cerr << h << std::endl;
        #endif
        if(!socketWrite(h.data(),h.size()))
            std::cerr << "ERROR to write: " << h << " errno: " << errno << std::endl;
    }
    /*used for retry host.clear();
    uri.clear();*/
}

char Http::randomETagChar(uint8_t r)
{
    #ifdef DEBUGFASTCGI
    if(rCharSize!=65)
        std::cerr << __FILE__ << ":" << __LINE__ << " wrong rChar size abort" << std::endl;
    #endif
    return rChar[r%rCharSize];
}

void Http::readyToRead()
{
/*    if(var=="content-length")
    if(var=="content-type")*/
    //::read(Http::buffer

    //load into buffer the previous content

    if(backend!=nullptr && /*if file end send*/ endDetected)
    {
        std::cerr << "Received data while not connected to http" << std::endl;
        while(socketRead(Http::buffer,sizeof(Http::buffer))>0)
        {}
        return;
    }

    uint16_t offset=0;
    if(!headerBuff.empty())
    {
        offset=headerBuff.size();
        memcpy(buffer,headerBuff.data(),headerBuff.size());
    }

    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    ssize_t readSize=0;
    do
    {
        errno=0;
        const ssize_t size=socketRead(buffer+offset,sizeof(buffer)-offset);
        readSize=size;
        #ifdef DEBUGFASTCGI
        std::cout << __FILE__ << ":" << __LINE__ << " " << readSize << std::endl;
        #endif
        if(size>0)
        {
            lastReceivedBytesTimestamps=Backend::currentTime();
            //std::cout << std::string(buffer,size) << std::endl;
            if(parsing==Parsing_Content)
            {
                write(buffer,size);
                if(endDetected)
                    return;
            }
            else
            {
                uint16_t pos=0;
                if(http_code==0)
                {
                    //HTTP/1.1 200 OK
                    void *fh=nullptr;
                    while(pos<size && buffer[pos]!='\n')
                    {
                        char &c=buffer[pos];
                        if(http_code==0 && c==' ')
                        {
                            if(fh==nullptr)
                            {
                                pos++;
                                fh=buffer+pos;
                            }
                            else
                            {
                                c=0x00;
                                http_code=atoi((char *)fh);
                                #ifdef DEBUGFASTCGI
                                std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                #endif
                                if(backend!=nullptr)
                                    backend->wasTCPConnected=true;
                                if(!HttpReturnCode(http_code))
                                {
                                    flushRead();
                                    #ifdef DEBUGFASTCGI
                                    std::cout << __FILE__ << ":" << __LINE__ << std::endl;
                                    #endif
                                    return;
                                }
                                pos++;
                            }
                        }
                        else
                            pos++;
                    }
                }
                if(http_code!=200)
                {
                    flushRead();
                    #ifdef DEBUGFASTCGI
                    std::cout << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                    return;
                }
                #ifdef DEBUGFASTCGI
                std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                #endif
                pos++;

                parsing=Parsing_HeaderVar;
                uint16_t pos2=pos;
                //content-length: 5000
                if(http_code!=0)
                {
                    while(pos<size)
                    {
                        char &c=buffer[pos];
                        if(c==':' && parsing==Parsing_HeaderVar)
                        {
                            if((pos-pos2)==4)
                            {
                                std::string var(buffer+pos2,pos-pos2);
                                std::transform(var.begin(), var.end(), var.begin(),[](unsigned char c){return std::tolower(c);});
                                if(var=="etag")
                                {
                                    parsing=Parsing_ETag;
                                    pos++;
                                    #ifdef DEBUGFASTCGI
                                    std::cout << "get backend etag" << std::endl;
                                    #endif
                                }
                                else
                                {
                                    parsing=Parsing_HeaderVal;
                                    //std::cout << "1a) " << std::string(buffer+pos2,pos-pos2) << " (" << pos-pos2 << ")" << std::endl;
                                    pos++;
                                }
                            }
                            #ifdef HTTPGZIP
                            else if((pos-pos2)==16)
                            {
                                std::string var(buffer+pos2,pos-pos2);
                                std::transform(var.begin(), var.end(), var.begin(),[](unsigned char c){return std::tolower(c);});
                                if(var=="content-encoding")
                                {
                                    parsing=Parsing_ContentEncoding;
                                    pos++;
                                    #ifdef DEBUGFASTCGI
                                    std::cout << "get backend content-encoding" << std::endl;
                                    #endif
                                }
                                else
                                {
                                    parsing=Parsing_HeaderVal;
                                    //std::cout << "1a) " << std::string(buffer+pos2,pos-pos2) << " (" << pos-pos2 << ")" << std::endl;
                                    pos++;
                                }
                            }
                            #endif
                            else if((pos-pos2)==14)
                            {
                                std::string var(buffer+pos2,pos-pos2);
                                std::transform(var.begin(), var.end(), var.begin(),[](unsigned char c){return std::tolower(c);});
                                if(var=="content-length")
                                {
                                    parsing=Parsing_ContentLength;
                                    pos++;
                                    #ifdef DEBUGFASTCGI
                                    std::cout << "get backend content-length" << std::endl;
                                    #endif
                                }
                                else
                                {
                                    parsing=Parsing_HeaderVal;
                                    //std::cout << "1a) " << std::string(buffer+pos2,pos-pos2) << " (" << pos-pos2 << ")" << std::endl;
                                    pos++;
                                }
                            }
                            else if((pos-pos2)==12)
                            {
                                std::string var(buffer+pos2,pos-pos2);
                                std::transform(var.begin(), var.end(), var.begin(),[](unsigned char c){return std::tolower(c);});
                                if(var=="content-type")
                                {
                                    parsing=Parsing_ContentType;
                                    pos++;
                                    #ifdef DEBUGFASTCGI
                                    std::cout << "get backend content-type" << std::endl;
                                    #endif
                                }
                                else
                                {
                                    parsing=Parsing_HeaderVal;
                                    //std::cout << "1a) " << std::string(buffer+pos2,pos-pos2) << " (" << pos-pos2 << ")" << std::endl;
                                    pos++;
                                }
                            }
                            else
                            {
                                parsing=Parsing_HeaderVal;
                                //std::cout << "1a) " << std::string(buffer+pos2,pos-pos2) << " (" << pos-pos2 << ")" << std::endl;
                                pos++;
                            }
                            if(c=='\r')
                            {
                                pos++;
                                const char &c2=buffer[pos];
                                if(c2=='\n')
                                    pos++;
                            }
                            else
                                pos++;
                            pos2=pos;
                        }
                        else if(c=='\n' || c=='\r')
                        {
                            if(pos==pos2 && parsing==Parsing_HeaderVar)
                            {
                                //end of header
                                #ifdef DEBUGFASTCGI
                                std::cout << "end of header" << std::endl;
                                #endif
                                parsing=Parsing_Content;
                                if(c=='\r')
                                {
                                    pos++;
                                    const char &c2=buffer[pos];
                                    if(c2=='\n')
                                        pos++;
                                }
                                else
                                    pos++;

                                //long filetime=0;
                        /*        long http_code = 0;
                                Http_easy_getinfo (easy, HttpINFO_RESPONSE_CODE, &http_code);
                                if(http_code==304) //when have header 304 Not Modified
                                {
                                    //set_last_modification_time_check() call before
                                    for(Client * client : clientsList)
                                        client->startRead(cachePath,false);
                                    return size;
                                }

                                Httpcode res = Http_easy_getinfo(easy, HttpINFO_FILETIME, &filetime);
                                if((HttpE_OK != res))
                                    filetime=0;*/
                                if(tempCache!=nullptr)
                                {
                                    tempCache=nullptr;
                                    delete tempCache;
                                }
                                #ifdef DEBUGFASTCGI
                                std::cout << "open((cachePath+.tmp).c_str() " << (cachePath+".tmp") << std::endl;
                                #endif
                                ::unlink((cachePath+".tmp").c_str());
                                int cachefd = open((cachePath+".tmp").c_str(), O_RDWR | O_CREAT | O_TRUNC/* | O_NONBLOCK*/, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                                if(cachefd==-1)
                                {
                                    if(errno==2)
                                    {
                                        ::mkdir("cache/",S_IRWXU);
                                        if(Cache::hostsubfolder)
                                        {
                                            const std::string::size_type &n=cachePath.rfind("/");
                                            const std::string basePath=cachePath.substr(0,n);
                                            mkdir(basePath.c_str(),S_IRWXU);
                                        }
                                        ::unlink((cachePath+".tmp").c_str());
                                        cachefd = open((cachePath+".tmp").c_str(), O_RDWR | O_CREAT | O_TRUNC/* | O_NONBLOCK*/, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                                        if(cachefd==-1)
                                        {
                                            #ifdef DEBUGFASTCGI
                                            std::cout << "open((cachePath+.tmp).c_str() failed " << (cachePath+".tmp") << " errno " << errno << std::endl;
                                            #endif
                                            //return internal error
                                            for(Client * client : clientsList)
                                            {
                                                #ifdef DEBUGFASTCGI
                                                std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                                #endif
                                                client->cacheError();
                                                client->disconnect();
                                            }
                                            disconnectFrontend();
                                            #ifdef DEBUGFASTCGI
                                            std::cout << __FILE__ << ":" << __LINE__ << std::endl;
                                            #endif
                                            return;
                                        }
                                        else
                                        {
                                            Cache::newFD(cachefd,this,EpollObject::Kind::Kind_Cache);
                                            #ifdef DEBUGFILEOPEN
                                            std::cerr << "Http::readyToRead() open: " << cachePath << ", fd: " << cachefd << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                            #endif
                                        }
                                    }
                                    else
                                    {
                                        #ifdef DEBUGFASTCGI
                                        std::cout << "open((cachePath+.tmp).c_str() failed " << (cachePath+".tmp") << " errno " << errno << std::endl;
                                        #endif
                                        //return internal error
                                        disconnectFrontend();
                                        #ifdef DEBUGFASTCGI
                                        std::cout << __FILE__ << ":" << __LINE__ << std::endl;
                                        #endif
                                        return;
                                    }
                                }
                                else
                                {
                                    Cache::newFD(cachefd,this,EpollObject::Kind::Kind_Cache);
                                    #ifdef DEBUGFILEOPEN
                                    std::cerr << "Http::readyToRead() open: " << (cachePath+".tmp") << ", fd: " << cachefd << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                    #endif
                                }

                                tempCache=new Cache(cachefd);
                                std::string r;
                                char randomIndex[6];
                                read(Http::fdRandom,randomIndex,sizeof(randomIndex));
                                {
                                    size_t rIndex=0;
                                    while(rIndex<6)
                                    {
                                        const char &c=randomETagChar(randomIndex[rIndex]);
                                        if(c==0x00 || c=='\0')
                                            std::cerr << "etag will contain \\0 abort" << __FILE__ << ":" << __LINE__ << std::endl;
                                        r+=c;
                                        rIndex++;
                                    }
                                }

                                const int64_t &currentTime=time(NULL);
                                if(!tempCache->set_access_time(currentTime))
                                {
                                    tempCache->close();
                                    delete tempCache;
                                    tempCache=nullptr;
                                    disconnectFrontend();
                                    disconnectBackend();
                                    for(Client * client : clientsList)
                                        client->writeEnd();
                                    clientsList.clear();
                                    return;
                                }
                                if(!tempCache->set_last_modification_time_check(currentTime))
                                {
                                    tempCache->close();
                                    delete tempCache;
                                    tempCache=nullptr;
                                    disconnectFrontend();
                                    disconnectBackend();
                                    for(Client * client : clientsList)
                                        client->writeEnd();
                                    clientsList.clear();
                                    std::cerr << this << " drop corrupted cache " << cachePath << ".tmp";
                                    {
                                        struct stat sb;
                                        const int rstat=fstat(cachefd,&sb);
                                        if(rstat==0 && sb.st_size>=0)
                                            std::cerr << " size: " << sb.st_size;
                                    }
                                    std::cerr << std::endl;
                                    ::unlink((cachePath+".tmp").c_str());//drop corrupted cache
                                    return;
                                }
                                if(!tempCache->set_http_code(http_code))
                                {
                                    tempCache->close();
                                    delete tempCache;
                                    tempCache=nullptr;
                                    disconnectFrontend();
                                    disconnectBackend();
                                    for(Client * client : clientsList)
                                        client->writeEnd();
                                    clientsList.clear();
                                    return;
                                }
                                if(!tempCache->set_ETagFrontend(r))//string of 6 char
                                {
                                    tempCache->close();
                                    delete tempCache;
                                    tempCache=nullptr;
                                    disconnectFrontend();
                                    disconnectBackend();
                                    for(Client * client : clientsList)
                                        client->writeEnd();
                                    clientsList.clear();
                                    return;
                                }
                                if(!tempCache->set_ETagBackend(etagBackend))//at end seek to content pos
                                {
                                    tempCache->close();
                                    delete tempCache;
                                    tempCache=nullptr;
                                    disconnectFrontend();
                                    disconnectBackend();
                                    for(Client * client : clientsList)
                                        client->writeEnd();
                                    clientsList.clear();
                                    return;
                                }

                                std::string header;
                                switch(http_code)
                                {
                                    case 200:
                                    break;
                                    case 404:
                                    header="Status: 404 NOT FOUND\n";
                                    break;
                                    default:
                                    header="Status: 500 Internal Server Error\n";
                                    break;
                                }
                                if(contentsize>=0)
                                    header+="Content-Length: "+std::to_string(contentsize)+"\n";
                                /*else
                                    header+="Transfer-Encoding: chunked\n";*/
                                #ifdef HTTPGZIP
                                if(!contentEncoding.empty())
                                {
                                    header+="Content-Encoding: "+contentEncoding+"\n";
                                    contentEncoding.clear();
                                }
                                #endif
                                if(!contenttype.empty())
                                    header+="Content-Type: "+contenttype+"\n";
                                else
                                    header+="Content-Type: text/html\n";
                                if(http_code==200)
                                {
                                    const std::string date=timestampsToHttpDate(currentTime);
                                    const std::string expire=timestampsToHttpDate(currentTime+Cache::timeToCache(http_code));
                                    header+="Date: "+date+"\n"
                                        "Expires: "+expire+"\n"
                                        "Cache-Control: public\n"
                                        "ETag: \""+r+"\"\n"
                                        "Access-Control-Allow-Origin: *\n";
                                }
                                #ifdef DEBUGFASTCGI
                                //std::cout << "header: " << header << std::endl;
                                #endif
                                header+="\n";
                                tempCache->seekToContentPos();
                                if(headerWriten)
                                {
                                    std::cerr << "headerWriten already to true, critical error (abort)" << std::endl;
                                    abort();
                                }
                                else
                                {
                                    headerWriten=true;
                                    if(tempCache->write(header.data(),header.size())!=(ssize_t)header.size())
                                    {
                                        std::cerr << "Header creation failed, abort to debug " << __FILE__ << ":" << __LINE__ << host << uri << " " << cachePath << std::endl;
                                        tempCache->close();
                                        delete tempCache;
                                        tempCache=nullptr;
                                        disconnectFrontend();
                                        disconnectBackend();
                                        for(Client * client : clientsList)
                                            client->writeEnd();
                                        clientsList.clear();
                                    }
                                    else
                                    {
                                        epoll_event event;
                                        memset(&event,0,sizeof(event));
                                        event.data.ptr = tempCache;
                                        event.events = EPOLLOUT | EPOLLRDHUP | EPOLLHUP | EPOLLRDHUP | EPOLLERR;
                                        //std::cerr << "EPOLL_CTL_ADD bis: " << cachefd << std::endl;

                                        //tempCache->setAsync(); -> to hard for now

                                        for(Client * client : clientsList)
                                            client->startRead(cachePath+".tmp",true);
                                    }
                                }
                                break;
                            }
                            else
                            {
                                switch(parsing)
                                {
                                    case Parsing_ContentLength:
                                    {
                                        uint64_t value64;
                                        std::istringstream iss(std::string(buffer+pos2,pos-pos2));
                                        iss >> value64;
                                        contentsize=value64;
                                        #ifdef DEBUGFASTCGI
                                        std::cout << "content-length: " << value64 << std::endl;
                                        #endif
                                    }
                                    break;
                                    case Parsing_ContentType:
                                        contenttype=std::string(buffer+pos2,pos-pos2);
                                    break;
                                    case Parsing_ETag:
                                        etagBackend=std::string(buffer+pos2,pos-pos2);
                                    break;
                                    #ifdef HTTPGZIP
                                    case Parsing_ContentEncoding:
                                        contentEncoding=std::string(buffer+pos2,pos-pos2);
                                    break;
                                    #endif
                                    default:
                                    //std::cout << "1b) " << std::string(buffer+pos2,pos-pos2) << std::endl;
                                    break;
                                }
                                parsing=Parsing_HeaderVar;
                            }
                            if(c=='\r')
                            {
                                pos++;
                                const char &c2=buffer[pos];
                                if(c2=='\n')
                                    pos++;
                            }
                            else
                                pos++;
                            pos2=pos;

                        }
                        else
                        {
                            //std::cout << c << std::endl;
                            if(c=='\r')
                            {
                                pos++;
                                const char &c2=buffer[pos];
                                if(c2=='\n')
                                    pos++;
                            }
                            else
                                pos++;
                        }
                    }
                }
                #ifdef DEBUGFASTCGI
                std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                #endif
                if(parsing==Parsing_Content)
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                    //std::cerr << "content: " << std::string(buffer+pos,size-pos) << std::endl;
                    if(size<=pos)
                        return;
                    const size_t finalSize=size-pos;
                    const size_t rSize=write(buffer+pos,finalSize);
                    if(endDetected || rSize<=0 || rSize!=finalSize)
                        return;
                }
                else
                {
                    switch(parsing)
                    {
                        case Parsing_HeaderVar:
                        case Parsing_ContentType:
                        case Parsing_ContentLength:
                            if(headerBuff.empty() && pos2>0)
                                headerBuff=std::string(buffer+pos2,pos-pos2);
                            else
                            {
                                switch(parsing)
                                {
                                    case Parsing_ContentLength:
                                    case Parsing_ContentType:
                                        parsing=Parsing_HeaderVar;
                                        readyToRead();
                                    break;
                                    default:
                                    std::cerr << "parsing var before abort over size: " << (int)parsing << std::endl;
                                    break;
                                }
                            }
                        break;
                        default:
                        std::cerr << "parsing var before abort over size: " << (int)parsing << std::endl;
                        break;
                    }
                }
                #ifdef DEBUGFASTCGI
                std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                #endif
            }
            /*const char *ptr = strchr(buffer,':',size);
            if(ptr==nullptr)
            {}
            else
            {
                if(header.empty())
                {
                    if((ptr-buffer)==sizeof("content-length") && memcmp(buffer,"content-length",sizeof("content-length"))==0)
                    {}
                    //if(var=="content-type")
                }
            }*/
        }
        else
        {
            if(errno!=11 && errno!=0)
            {
                const auto p1 = std::chrono::system_clock::now();
                std::cout << std::chrono::duration_cast<std::chrono::seconds>(
                                p1.time_since_epoch()).count() << " socketRead(), errno " << errno << " for " << getUrl() << std::endl;
            }
            break;
        }
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
    } while(readSize>0);
    #ifdef DEBUGFASTCGI
    std::cout << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
}

void Http::readyToWrite()
{
    #ifdef DEBUGFASTCGI
    if(uri.empty())
    {
        std::cerr << "Http::readyToWrite(): but uri.empty() " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
        return;
    }
    #endif
    if(!requestSended)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " Http::readyToWrite() uri: " << uri << std::endl;
        #endif
        sendRequest();
    }
}

ssize_t Http::socketRead(void *buffer, size_t size)
{
    if(backend==nullptr)
    {
        std::cerr << "Http::socketRead error backend==nullptr" << std::endl;
        return -1;
    }
    if(!backend->isValid())
    {
        std::cerr << "Http::socketRead error backend is not valid: " << backend << std::endl;
        return -1;
    }
    return backend->socketRead(buffer,size);
}

bool Http::socketWrite(const void *buffer, size_t size)
{
    if(backend==nullptr)
    {
        std::cerr << "Http::socketWrite error backend==nullptr" << std::endl;
        return false;
    }
    if(!backend->isValid())
    {
        std::cerr << "Http::socketWrite error backend is not valid: " << backend << std::endl;
        return false;
    }
    return backend->socketWrite(buffer,size);
}

std::unordered_map<std::string,Http *> &Http::pathToHttpList()
{
    return Http::pathToHttp;
}

//always call disconnectFrontend() before disconnectBackend()
void Http::disconnectFrontend()
{
    #ifdef DEBUGFASTCGI
    std::cerr << "disconnectFrontend " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << " cachePath: " << cachePath << std::endl;
    #endif
    for(Client * client : clientsList)
    {
        client->writeEnd();
        client->disconnect();
    }
    clientsList.clear();
    //disconnectSocket();

    #ifdef DEBUGFASTCGI
    for(const Client * client : Client::clients)
    {
        if(client->http==this)
        {
            std::cerr << "Http::~Http(): destructor, remain client on this http " << __FILE__ << ":" << __LINE__ << std::endl;
            abort();
        }
    }
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
    if(cachePath.empty())
        std::cerr << "Http::disconnectFrontend() cachePath.empty()" << std::endl;
    else
    {
        std::unordered_map<std::string,Http *> &pathToHttp=pathToHttpList();
        if(pathToHttp.find(cachePath)==pathToHttp.cend())
            std::cerr << "Http::pathToHttp.find(" << cachePath << ")==Http::pathToHttp.cend()" << std::endl;
    }
    #endif
    /* generate: ./Backend.cpp:344 http 0x68d2630 is finished, will be destruct
./Backend.cpp:421
0x68d2630: http->backend=null and !backendList->pending.empty()
Http::backendError(Internal error, !haveUrlAndFrontendConnected), but pathToHttp.find(cache/13C1FCE29C43F20D) not found (abort) 0x68d3920
     *
     * if(!cachePath.empty())
    {
        std::unordered_map<std::string,Http *> &pathToHttp=pendingList();
        if(pathToHttp.find(cachePath)!=pathToHttp.cend())
        {
            std::cerr << "Http::disconnectFrontend(), erase pathToHttp.find(" << cachePath << ") " << this << std::endl;
            pathToHttp.erase(cachePath);
        }
        #ifdef DEBUGFASTCGI
        else
            std::cerr << this << " disconnectFrontend cachePath not found: " << cachePath << " " << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
    }
    #ifdef DEBUGFASTCGI
    else
        std::cerr << this << " disconnectFrontend cachePath not found: " << cachePath << " " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif*/
    /* can be in progress on backend {
        std::string cachePathTmp=cachePath+".tmp";
        if(!cachePathTmp.empty())
        {
            std::unordered_map<std::string,Http *> &pathToHttp=pendingList();
            if(pathToHttp.find(cachePathTmp)!=pathToHttp.cend())
                pathToHttp.erase(cachePathTmp);
            #ifdef DEBUGFASTCGI
            else
                std::cerr << this << " disconnectFrontend cachePath not found: " << cachePathTmp << " " << __FILE__ << ":" << __LINE__ << std::endl;
            #endif
        }
        #ifdef DEBUGFASTCGI
        else
            std::cerr << this << " disconnectFrontend cachePath not found: " << cachePathTmp << " " << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
    }*/

    url.clear();
    headerBuff.clear();

    if(!contenttype.empty())
    {
        contenttype.clear();
        /*if(backend==nullptr && clientsList.empty() && !isAlive() && contenttype.empty())
            Http::toDelete.insert(this);*/
    }
}

bool Http::haveUrlAndFrontendConnected() const
{
    return !host.empty() && !uri.empty() && !clientsList.empty();
}

bool Http::isAlive() const
{
    return !host.empty() && !uri.empty();
}

bool Http::HttpReturnCode(const int &errorCode)
{
    if(errorCode==200)
        return true;
    if(errorCode==304) //when have header 304 Not Modified
    {
        #ifdef DEBUGFASTCGI
        std::cout << "304 http code!, cache already good" << std::endl;
        #endif
        bool ok=false;
        if(finalCache!=nullptr)
        {
            if(!finalCache->set_last_modification_time_check(time(NULL)))
            {
                std::cerr << this << " drop corrupted cache " << cachePath << ".tmp";
                {
                    struct stat sb;
                    const int rstat=fstat(finalCache->getFD(),&sb);
                    if(rstat==0 && sb.st_size>=0)
                        std::cerr << " size: " << sb.st_size;
                }
                std::cerr << std::endl;
                ::unlink((cachePath+".tmp").c_str());//drop corrupted cache
            }
            else
                ok=true;
        }
        //send file to listener
        if(ok)
        {
            for(Client * client : clientsList)
            {
                #ifdef DEBUGFASTCGI
                std::cout << "send file to listener: " << client << std::endl;
                #endif
                client->startRead(cachePath,false);
            }
            return false;
        }
    }
    const std::string errorString("Http "+std::to_string(errorCode));
    for(Client * client : clientsList)
        client->httpError(errorString);
    clientsList.clear();
    return false;
    //disconnectSocket();
}

bool Http::backendError(const std::string &errorString)
{
    for(Client * client : clientsList)
        client->httpError(errorString);
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " Http::backendError(" << errorString << "), erase pathToHttp.find(" << cachePath << ") " << this << std::endl;
    #endif
    clientsList.clear();
    if(!cachePath.empty())
    {
        std::unordered_map<std::string,Http *> &pathToHttp=pathToHttpList();
        #ifdef DEBUGFASTCGI
        if(tempCache!=nullptr)
        {
            if(pathToHttp.find(cachePath+".tmp")==pathToHttp.cend())
            {
                std::cerr << "Http::backendError(" << errorString << "), but pathToHttp.find(" << cachePath+".tmp" << ") not found (abort) " << this << std::endl;
                abort();
            }
            else if(pathToHttp.at(cachePath+".tmp")!=this)
            {
                std::cerr << "Http::backendError(" << errorString << "), but pathToHttp.find(" << cachePath+".tmp" << ")!=this (abort) " << this << std::endl;
                abort();
            }
            else
                std::cerr << "Http::backendError(" << errorString << "), erase pathToHttp.find(" << cachePath+".tmp" << ") " << this << std::endl;
        }
        if(finalCache!=nullptr)
        {
            if(pathToHttp.find(cachePath)==pathToHttp.cend())
            {
                std::cerr << "Http::backendError(" << errorString << "), but pathToHttp.find(" << cachePath << ") not found (abort) " << this << std::endl;
                abort();
            }
            else if(pathToHttp.at(cachePath)!=this)
            {
                std::cerr << "Http::backendError(" << errorString << "), but pathToHttp.find(" << cachePath << ")!=this (abort) " << this << std::endl;
                abort();
            }
            else
                std::cerr << "Http::backendError(" << errorString << "), erase pathToHttp.find(" << cachePath << ") " << this << std::endl;
        }
        #endif
        //if(finalCache!=nullptr) -> file descriptor can be NOT open due to timeout while Http object is in pending queue
        {
            #ifdef DEBUGFASTCGI
            if(&pathToHttp==&Http::pathToHttp)
                std::cerr << "pathToHttp.erase(" << cachePath << ") " << this << std::endl;
            if(&pathToHttp==&Https::pathToHttps)
                std::cerr << "pathToHttps.erase(" << cachePath << ") " << this << std::endl;
            #endif
            pathToHttp.erase(cachePath);
        }
        //if(tempCache!=nullptr) -> file descriptor can be NOT open due to timeout while Http object is in pending queue
        {
            #ifdef DEBUGFASTCGI
            if(&pathToHttp==&Http::pathToHttp)
                std::cerr << "pathToHttp.erase(" << cachePath+".tmp" << ") " << this << std::endl;
            if(&pathToHttp==&Https::pathToHttps)
                std::cerr << "pathToHttps.erase(" << cachePath+".tmp" << ") " << this << std::endl;
            #endif
            pathToHttp.erase(cachePath+".tmp");
        }
        cachePath.clear();
    }
    #ifdef DEBUGFASTCGI
    {
        std::unordered_map<std::string/* example: cache/29E7336BDEA3327B */,Http *> pathToHttp=Http::pathToHttp;
        for( const auto &n : pathToHttp )
            if(n.second==this)
            {
                std::cerr << "Http::~Http(): destructor post opt this " << this << " can't be into Http::pathToHttp at " << n.first << " " << __FILE__ << ":" << __LINE__ << " cachePath: " << cachePath << std::endl;
                abort();
            }
    }
    {
        std::unordered_map<std::string/* example: cache/29E7336BDEA3327B */,Http *> pathToHttp=Https::pathToHttps;
        for( const auto &n : pathToHttp )
            if(n.second==this)
            {
                std::cerr << "Http::~Http(): destructor post opt this " << this << " can't be into Https::pathToHttps at " << n.first << " " << __FILE__ << ":" << __LINE__ << " cachePath: " << cachePath << std::endl;
                abort();
            }
    }
    #endif
    return false;
    //disconnectSocket();
}

std::string Http::getUrl() const
{
    if(host.empty() && uri.empty())
        return "no url";
    else
        return "http://"+host+uri;
}

void Http::flushRead()
{
    disconnectBackend();
    disconnectFrontend();
    while(socketRead(Http::buffer,sizeof(Http::buffer))>0)
    {}
}

void Http::parseNonHttpError(const Backend::NonHttpError &error)
{
    switch(error)
    {
        case Backend::NonHttpError_AlreadySend:
        {
            const std::string &errorString("Tcp request already send (internal error)");
            for(Client * client : clientsList)
                client->httpError(errorString);
        }
        break;
        case Backend::NonHttpError_Timeout:
        {
            const std::string &errorString("Http timeout, too many time without data (internal error)");
            for(Client * client : clientsList)
                client->httpError(errorString);
        }
        break;
        default:
        {
            const std::string &errorString("Unknown non HTTP error");
            for(Client * client : clientsList)
                client->httpError(errorString);
        }
        break;
    }
}

//always call disconnectFrontend() before disconnectBackend()
void Http::disconnectBackend(const bool fromDestructor)
{
    #ifdef DEBUGFASTCGI
    std::cerr << "Http::disconnectBackend() " << this << std::endl;
    #endif

    #ifdef DEBUGFILEOPEN
    std::cerr << "Http::disconnectBackend() post, finalCache close: " << finalCache << std::endl;
    #endif
    if(finalCache!=nullptr)
        finalCache->close();
    const char * const cstr=cachePath.c_str();
    //todo, optimise with renameat2(RENAME_EXCHANGE) if --flatcache + destination
    #ifdef DEBUGFILEOPEN
    std::cerr << "Http::disconnectBackend() post, tempCache close: " << tempCache << std::endl;
    #endif
    if(tempCache!=nullptr)
    {
        const ssize_t &tempSize=tempCache->size();
        if(tempSize<25)
        {
            std::cerr << (cachePath+".tmp") << " corrupted temp file (abort)" << std::endl;
            ::unlink(cstr);
            ::unlink((cachePath+".tmp").c_str());
            abort();
        }
        #ifdef DEBUGFASTCGI
        std::cerr << (cachePath+".tmp") << " temp file size: " << tempSize << std::endl;
        #endif
        tempCache->close();
        struct stat sb;
        const int rstat=stat((cachePath+".tmp").c_str(),&sb);
        if(rstat==0)
        {
            if(sb.st_size<100000000)
            {
                if(sb.st_size>25)
                {
                    ::unlink(cstr);
                    if(rename((cachePath+".tmp").c_str(),cstr)!=0)
                    {
                        if(errno==2)
                        {
                            ::mkdir("cache/",S_IRWXU);
                            if(Cache::hostsubfolder)
                            {
                                const std::string::size_type &n=cachePath.rfind("/");
                                const std::string basePath=cachePath.substr(0,n);
                                mkdir(basePath.c_str(),S_IRWXU);
                            }
                            if(rename((cachePath+".tmp").c_str(),cstr)!=0)
                            {
                                std::cerr << "unable to move " << cachePath << ".tmp to " << cachePath << ", errno: " << errno << std::endl;
                                ::unlink((cachePath+".tmp").c_str());
                            }
                        }
                        else
                        {
                            std::cerr << "unable to move " << cachePath << ".tmp to " << cachePath << ", errno: " << errno << std::endl;
                            ::unlink((cachePath+".tmp").c_str());
                        }
                    }
                }
                else
                {
                    std::cerr << "Too small to be saved (abort): " << cachePath+".tmp" << " " << __FILE__ << ":" << __LINE__ << std::endl;
                    ::unlink(cstr);
                    ::unlink((cachePath+".tmp").c_str());
                }
            }
            else
            {
                std::cerr << "Too big to be saved (abort): " << cachePath+".tmp" << " " << __FILE__ << ":" << __LINE__ << std::endl;
                ::unlink(cstr);
                ::unlink((cachePath+".tmp").c_str());
            }
        }
        else
        {
            std::cerr << "Not found: " << cachePath+".tmp" << " " << __FILE__ << ":" << __LINE__ << std::endl;
            ::unlink(cstr);
            ::unlink((cachePath+".tmp").c_str());
        }
        //disable to cache
        if(!Cache::enable)
            ::unlink(cstr);
    }

    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " disconnect http " << this << " from backend " << backend << std::endl;
    #endif
    //remove from busy, should never be into idle
    if(backend!=nullptr)
        backend->downloadFinished();
    else
    {
        //remove from pending
        if(backendList!=nullptr)
        {
            unsigned int index=0;
            while(index<backendList->pending.size())
            {
                if(backendList->pending.at(index)==this)
                    break;
                index++;
            }
            if(index>=backendList->pending.size())
                std::cerr << this << " backend==nullptr and this " << this << " not found into pending, isAlive(): " << std::to_string((int)isAlive()) << ", clientsList size: " << std::to_string(clientsList.size()) << " (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
            else
                backendList->pending.erase(backendList->pending.cbegin()+index);
        }
        else
            std::cerr << this << " backendList==nullptr and this " << this << " take care " << __FILE__ << ":" << __LINE__ << std::endl;
    }
    #ifdef DEBUGFASTCGI
    if(backend!=nullptr && backend->http==this)
    {
        std::cerr << this << ": backend->http==this, backend: " << backend << " (abort)" << std::endl;
        abort();
    }
    std::cerr << this << ": backend=nullptr" << std::endl;

    //if this can be located into another backend, then error
    for( const auto& n : Backend::addressToHttp )
    {
        const Backend::BackendList * list=n.second;
        for(const Backend * b : list->busy)
            if(b->http==this)
            {
                std::cerr << this << ": backend->http==this, backend http: " << backend << " " << getUrl() << " (abort)" << std::endl;
                //abort();//why this is an error?
            }
    }
    for( const auto& n : Backend::addressToHttps )
    {
        const Backend::BackendList * list=n.second;
        for(const Backend * b : list->busy)
            if(b->http==this)
            {
                std::cerr << this << ": backend->http==this, backend https: " << backend << " " << getUrl() << " (abort)" << std::endl;
                //abort();//why this is an error?
            }
    }
    #endif
    backend=nullptr;
    backendList=nullptr;
    //Http::toDelete.insert(this);

    #ifdef DEBUGFASTCGI
    std::cerr << "disconnectBackend " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__
        << " backend: " << (void *)backend
        << " clientsList size: " << std::to_string((int)clientsList.size())
        << " isAlive(): " << (int)isAlive()
        << " contenttype.size(): " << std::to_string((int)contenttype.size())
        << std::endl;
    #endif

    if(backend==nullptr && isAlive()
            /* contenttype.empty() -> empty if never try download due to timeout*/
            )
    {
        #ifdef DEBUGFASTCGI
        if(!clientsList.empty())
            std::cerr << "disconnectBackend " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__
                << " WARNING clientsList size: " << std::to_string((int)clientsList.size())
                << " mostly in case of timeout before start to download"
                << std::endl;
        #endif
        if(!fromDestructor)
        {
            #ifdef DEBUGFASTCGI
            if(Http::toDebug.find(this)==Http::toDebug.cend())
            {
                std::cerr << this << " Http::toDelete.insert() failed because not into debug" << " " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
            else
                std::cerr << this << " Http::toDelete.insert() ok" << " " << __FILE__ << ":" << __LINE__ << std::endl;
            #endif
            Http::toDelete.insert(this);
            #ifdef DEBUGFASTCGI
            for(const Client * client : Client::clients)
            {
                if(client->http==this)
                {
                    std::cerr << "Http::~Http(): destructor, remain client on this http " << __FILE__ << ":" << __LINE__ << std::endl;
                    abort();
                }
            }
            for( const auto &n : Backend::addressToHttp )
            {
                for( const auto &m : n.second->busy )
                {
                    if(m->http==this)
                    {
                        std::cerr << (void *)m << " p->http==" << this << " into busy list, error http (abort)" << std::endl;
                        abort();
                    }
                }
                for( const auto &m : n.second->idle )
                {
                    if(m->http==this)
                    {
                        std::cerr << (void *)m << " p->http==" << this << " into busy list, error http (abort)" << std::endl;
                        abort();
                    }
                }
                for( const auto &m : n.second->pending )
                {
                    if(m==this)
                    {
                        std::cerr << (void *)m << " p->http==" << this << " into busy list, error http (abort)" << std::endl;
                        abort();
                    }
                }
            }
            for( const auto &n : Backend::addressToHttps )
            {
                for( const auto &m : n.second->busy )
                {
                    if(m->http==this)
                    {
                        std::cerr << (void *)m << " p->http==" << this << " into busy list, error http (abort)" << std::endl;
                        abort();
                    }
                }
                for( const auto &m : n.second->idle )
                {
                    if(m->http==this)
                    {
                        std::cerr << (void *)m << " p->http==" << this << " into busy list, error http (abort)" << std::endl;
                        abort();
                    }
                }
                for( const auto &m : n.second->pending )
                {
                    if(m==this)
                    {
                        std::cerr << (void *)m << " p->http==" << this << " into busy list, error http (abort)" << std::endl;
                        abort();
                    }
                }
            }
            #endif
        }
    }
    if(!cachePath.empty())
    {
        #ifdef DEBUGFASTCGI
        std::string pathToHttpVar="pathToHttp";
        if(&pathToHttpList()!=&Http::pathToHttp)
            pathToHttpVar="pathToHttps";
        #endif
        std::unordered_map<std::string,Http *> &pathToHttp=pathToHttpList();
        if(pathToHttp.find(cachePath)!=pathToHttp.cend())
        {
            #ifdef DEBUGFASTCGI
            if(pathToHttp.at(cachePath)!=this)
            {
                std::cerr << "Http::disconnectBackend(), but " << pathToHttpVar << ".find(" << cachePath << ") not found (abort) " << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
            #endif
            std::cerr << "Http::disconnectBackend(), erase " << pathToHttpVar << ".find(" << cachePath << ") " << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
            pathToHttp.erase(cachePath);
        }
        #ifdef DEBUGFASTCGI
        else
            std::cerr << this << " disconnectFrontend cachePath not found: " << cachePath << " " << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
        std::string cachePathTmp=cachePath+".tmp";
        if(pathToHttp.find(cachePathTmp)!=pathToHttp.cend())
        {
            #ifdef DEBUGFASTCGI
            if(pathToHttp.at(cachePathTmp)!=this)
            {
                std::cerr << "Http::disconnectBackend(), but " << pathToHttpVar << ".find(" << cachePathTmp << ") not found (abort) " << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
            #endif
            std::cerr << "Http::disconnectBackend(), erase " << pathToHttpVar << ".find(" << cachePathTmp << ") " << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
            pathToHttp.erase(cachePathTmp);
        }
        #ifdef DEBUGFASTCGI
        else
            std::cerr << this << " disconnectFrontend cachePath not found: " << cachePathTmp << " " << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
    }
    #ifdef DEBUGFASTCGI
    else
        std::cerr << this << " disconnectFrontend cachePath not found: " << cachePath << " " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    #ifdef DEBUGFASTCGI
    {
        std::unordered_map<std::string/* example: cache/29E7336BDEA3327B */,Http *> pathToHttp=Http::pathToHttp;
        for( const auto &n : pathToHttp )
            if(n.second==this)
            {
                std::cerr << "Http::~Http(): destructor post opt this " << this << " can't be into Http::pathToHttp at " << n.first << " " << __FILE__ << ":" << __LINE__ << " cachePath: " << cachePath << std::endl;
                abort();
            }
    }
    {
        std::unordered_map<std::string/* example: cache/29E7336BDEA3327B */,Http *> pathToHttp=Https::pathToHttps;
        for( const auto &n : pathToHttp )
            if(n.second==this)
            {
                std::cerr << "Http::~Http(): destructor post opt this " << this << " can't be into Https::pathToHttps at " << n.first << " " << __FILE__ << ":" << __LINE__ << " cachePath: " << cachePath << std::endl;
                abort();
            }
    }
    #endif

    cachePath.clear();
    host.clear();
    uri.clear();
    etagBackend.clear();
    lastReceivedBytesTimestamps=0;
    requestSended=false;
    endDetected=false;
}

void Http::addClient(Client * client)
{
    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " add client: " << client << " client fd: " << client->getFD() << std::endl;
    if(cachePath.empty())
        std::cerr << "addClient() cachePath.empty()" << std::endl;
    else
    {
        std::unordered_map<std::string,Http *> &pathToHttp=pathToHttpList();
        if(pathToHttp.find(cachePath)==pathToHttp.cend())
            std::cerr << "Http::pathToHttp.find(" << cachePath << ")==Http::pathToHttp.cend()" << std::endl;
    }
    #endif
    #ifdef DEBUGFASTCGI
    if(cachePath.empty())
    {
        {
            std::unordered_map<std::string/* example: cache/29E7336BDEA3327B */,Http *> pathToHttp=Http::pathToHttp;
            for( const auto &n : pathToHttp )
                if(n.second==this)
                {
                    std::cerr << "Http::~Http(): destructor post opt this " << this << " can't be into Http::pathToHttp at " << n.first << " " << __FILE__ << ":" << __LINE__ << " cachePath: " << cachePath << std::endl;
                    abort();
                }
        }
        {
            std::unordered_map<std::string/* example: cache/29E7336BDEA3327B */,Http *> pathToHttp=Https::pathToHttps;
            for( const auto &n : pathToHttp )
                if(n.second==this)
                {
                    std::cerr << "Http::~Http(): destructor post opt this " << this << " can't be into Https::pathToHttps at " << n.first << " " << __FILE__ << ":" << __LINE__ << " cachePath: " << cachePath << std::endl;
                    abort();
                }
        }
    }
    #endif
    if(host.empty() || uri.empty())
    {
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " Add client to dead http url downloader: " << client << std::endl;
        #endif
        client->httpError("Add client to dead http url downloader");
        return;
    }

    //drop performance, but more secure, remove when more stable
    size_t i=0;
    while(i<clientsList.size())
    {
        if(clientsList.at(i)==client)
        {
            #ifdef DEBUGFASTCGI
            std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " dual addClient detected: " << client << std::endl;
            #endif
            return;
        }
        i++;
    }

    clientsList.push_back(client);
    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    if(tempCache)
        client->startRead(cachePath+".tmp",true);
    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " backend " << backend << " cachePath: " << cachePath << std::endl;
    #endif
    // can be without backend asigned due to max backend
}

bool Http::removeClient(Client * client)
{
    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " remove client: " << client << " cachePath: " << cachePath << std::endl;
    #endif
    #ifdef DEBUGFASTCGI
    if(cachePath.empty())
    {
        {
            std::unordered_map<std::string/* example: cache/29E7336BDEA3327B */,Http *> pathToHttp=Http::pathToHttp;
            for( const auto &n : pathToHttp )
                if(n.second==this)
                {
                    std::cerr << "Http::~Http(): destructor post opt this " << this << " can't be into Http::pathToHttp at " << n.first << " " << __FILE__ << ":" << __LINE__ << " cachePath: " << cachePath << std::endl;
                    abort();
                }
        }
        {
            std::unordered_map<std::string/* example: cache/29E7336BDEA3327B */,Http *> pathToHttp=Https::pathToHttps;
            for( const auto &n : pathToHttp )
                if(n.second==this)
                {
                    std::cerr << "Http::~Http(): destructor post opt this " << this << " can't be into Https::pathToHttps at " << n.first << " " << __FILE__ << ":" << __LINE__ << " cachePath: " << cachePath << std::endl;
                    abort();
                }
        }
    }
    #endif
    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " remove client: " << client << " cachePath: " << cachePath << std::endl;
    #endif
    //some drop performance at exchange of bug prevent
    size_t i=0;
    size_t itemDropped=0;
    while(i<clientsList.size())
    {
        if(clientsList.at(i)==client)
        {
            clientsList.erase(clientsList.cbegin()+i);
            itemDropped++;
            //return true;
        }
        else
            i++;
    }
    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " remove client: " << client << " cachePath: " << cachePath << std::endl;
    #endif
    //return false;
    #ifdef DEBUGFASTCGI
    if(itemDropped!=1)
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " remove client failed: " << client << ", itemDropped: " << itemDropped << " cachePath: " << cachePath << std::endl;
    #endif
    return itemDropped==1;


    //std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " failed to remove: " << client << std::endl;
    /*auto p=std::find(clientsList.cbegin(),clientsList.cend(),client);
    if(p!=clientsList.cend())
        clientsList.erase(p);*/
}

int Http::write(const char * const data,const size_t &size)
{
    if(endDetected)
        return -1;
    if(tempCache==nullptr)
    {
        //std::cerr << "tempCache==nullptr internal error" << std::endl;
        return size;
    }

    if(contentsize>=0)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
        const size_t &writedSize=tempCache->write((char *)data,size);
        (void)writedSize;
        for(Client * client : clientsList)
            client->tryResumeReadAfterEndOfFile();
        contentwritten+=size;
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
        std::cerr << "contentsize: " << contentsize << ", contentwritten: " << contentwritten << std::endl;
        #endif
        if(contentsize<=contentwritten)
        {
            #ifdef DEBUGFASTCGI
            std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
            #endif
            disconnectFrontend();
            disconnectBackend();
            endDetected=true;
            return size;
        }
    }
    else
    {
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
        size_t pos=0;
        size_t pos2=0;
        //content-length: 5000
        if(http_code!=0)
        {
            #ifdef DEBUGFASTCGI
            std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
            #endif
            while(pos<size)
            {
                if(chunkLength>0)
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                    //std::cerr << "parsed) " << Common::binarytoHexa(data,pos) << std::endl;
                    //std::cerr << "NOT parsed) " << Common::binarytoHexa(data+pos,size-pos) << std::endl;
                    #endif
                    if((size_t)chunkLength>(size-pos))
                    {
                        const size_t &writedSize=tempCache->write((char *)data+pos,size-pos);
                        (void)writedSize;
                        for(Client * client : clientsList)
                            client->tryResumeReadAfterEndOfFile();
                        contentwritten+=size;
                        pos+=size-pos;
                        pos2=pos;
                    }
                    else
                    {
                        const size_t &writedSize=tempCache->write((char *)data+pos,chunkLength);
                        (void)writedSize;
                        for(Client * client : clientsList)
                            client->tryResumeReadAfterEndOfFile();
                        contentwritten+=chunkLength;
                        pos+=chunkLength;
                        pos2=pos;
                    }
                    chunkLength=-1;
                    #ifdef DEBUGFASTCGI
                    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                    //std::cerr << "parsed) " << Common::binarytoHexa(data,pos) << std::endl;
                    //std::cerr << "NOT parsed) " << Common::binarytoHexa(data+pos,size-pos) << std::endl;
                    #endif
                }
                else
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                    //std::cerr << "parsed) " << Common::binarytoHexa(data,pos) << std::endl;
                    //std::cerr << "NOT parsed) " << Common::binarytoHexa(data+pos,size-pos) << std::endl;
                    #endif
                    while((size_t)pos<size)
                    {
                        char c=data[pos];
                        if(c=='\n' || c=='\r')
                        {
                            #ifdef DEBUGFASTCGI
                            std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                            //std::cerr << "parsed) " << Common::binarytoHexa(data,pos) << std::endl;
                            //std::cerr << "NOT parsed) " << Common::binarytoHexa(data+pos,size-pos) << std::endl;
                            #endif
                            if(pos2==pos)
                            {
                                if(c=='\r')
                                {
                                    pos++;
                                    const char &c2=data[pos];
                                    if(c2=='\n')
                                        pos++;
                                }
                                else
                                    pos++;
                                pos2=pos;
                            }
                            else
                            {
                                #ifdef DEBUGFASTCGI
                                std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                #endif
                                if(chunkHeader.empty())
                                    chunkLength=Common::hexaTo64Bits(std::string(data+pos2,pos-pos2));
                                else
                                {
                                    #ifdef DEBUGFASTCGI
                                    std::cerr << "chunkHeader ban (abort)" << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                    abort();
                                    #endif
                                    chunkHeader+=std::string(data,pos-1);
                                    chunkLength=Common::hexaTo64Bits(chunkHeader);
                                }
                                #ifdef DEBUGFASTCGI
                                std::cerr << "chunkLength: " << chunkLength << std::endl;
                                #endif
                                #ifdef DEBUGFASTCGI
                                std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                //std::cerr << "parsed) " << Common::binarytoHexa(data,pos) << std::endl;
                                //std::cerr << "NOT parsed) " << Common::binarytoHexa(data+pos,size-pos) << std::endl;
                                #endif
                                if(c=='\r')
                                {
                                    pos++;
                                    const char &c2=data[pos];
                                    if(c2=='\n')
                                        pos++;
                                }
                                else
                                    pos++;
                                pos2=pos;
                                #ifdef DEBUGFASTCGI
                                std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                //std::cerr << "parsed) " << Common::binarytoHexa(data,pos) << std::endl;
                                //std::cerr << "NOT parsed) " << Common::binarytoHexa(data+pos,size-pos) << std::endl;
                                #endif
                                break;
                            }
                            #ifdef DEBUGFASTCGI
                            std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                            //std::cerr << "parsed) " << Common::binarytoHexa(data,pos) << std::endl;
                            //std::cerr << "NOT parsed) " << Common::binarytoHexa(data+pos,size-pos) << std::endl;
                            #endif
                        }
                        else
                            pos++;
                    }
                    #ifdef DEBUGFASTCGI
                    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                    if(chunkLength==0)
                    {
                        #ifdef DEBUGFASTCGI
                        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                        #endif
                        disconnectFrontend();
                        disconnectBackend();
                        endDetected=true;
                        /*if(c=='\r')
                        {
                            pos++;
                            const char &c2=data[pos];
                            if(c2=='\n')
                                pos++;
                        }
                        else
                            pos++;*/
                        return size;
                    }
                    else if((size_t)pos>=size && chunkLength<0 && pos2<pos)
                    {
                        #ifdef DEBUGFASTCGI
                        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                        #endif
                        if(chunkHeader.empty())
                        {
                            chunkHeader=std::string(data+pos2,size-pos2);
                        }
                        else
                        {
                            #ifdef DEBUGFASTCGI
                            std::cerr << "chunkHeader ban (abort)" << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                            abort();
                            #endif
                        }
                    }
                }
            }
        }
    }

    return size;
    //(write partial cache)
    //open to write .tmp (mv at end)
    //std::cout << std::string((const char *)data,size) << std::endl;
}

std::string Http::timestampsToHttpDate(const int64_t &time)
{
    char buffer[256];
    struct tm *my_tm = gmtime(&time);
    if(strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", my_tm)==0)
        return std::string("Thu, 1 Jan 1970 0:0:0 GMT");
    return buffer;
}

#ifdef DEBUGFASTCGI
void Http::checkBackend()
{
    if(backendList!=nullptr)
    {
        if(!backendList->idle.empty())
        {
            std::cerr << this << " backend==nullptr and !list->idle.empty(), isAlive(): " << std::to_string((int)isAlive()) << ", clientsList size: " << std::to_string(clientsList.size()) << " (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
            abort();
        }
        if(backendList->busy.size()<Backend::maxBackend)
        {
            std::cerr << this << " backend==nullptr and list->busy.size()<Backend::maxBackend, isAlive(): " << std::to_string((int)isAlive()) << ", clientsList size: " << std::to_string(clientsList.size()) << " (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
            abort();
        }
        unsigned int index=0;
        while(index<backendList->pending.size())
        {
            if(backendList->pending.at(index)==this)
                break;
            index++;
        }
        if(index>=backendList->pending.size())
        {
            std::cerr << this << " backend==nullptr and this " << this << " not found into pending, isAlive(): " << std::to_string((int)isAlive()) << ", clientsList size: " << std::to_string(clientsList.size()) << " (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
            abort();
        }
    }
    else
    {
        std::string host="Unknown IPv6";
        char str[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &m_socket.sin6_addr, str, INET6_ADDRSTRLEN) != NULL)
            host=str;
        std::cerr << this << " http backend==nullptr and no backend list found, isAlive(): " << std::to_string((int)isAlive()) << ", clientsList size: " << std::to_string(clientsList.size()) << " " << host << " (abort)" << std::endl;
        abort();
    }
}
#endif

//return true if timeout
bool Http::detectTimeout()
{
    const uint64_t msFrom1970=Backend::currentTime();
    unsigned int secondForTimeout=5;
    if(pending)
        secondForTimeout=30;

    if(lastReceivedBytesTimestamps>(msFrom1970-secondForTimeout*1000))
    {
        //prevent time drift
        if(lastReceivedBytesTimestamps>msFrom1970)
        {
            std::cerr << "Http::detectTimeout(), time drift" << std::endl;
            lastReceivedBytesTimestamps=msFrom1970;
        }

        #ifdef DEBUGFASTCGI
        //check here if not backend AND free backend or backend count < max
        if(backend==nullptr && (isAlive() || !clientsList.empty()))
        {
            //if have already connected backend on this ip
            checkBackend();
        }
        #endif
        return false;
    }
    if(backend!=nullptr)
        std::cerr << "Http::detectTimeout() need to quit " << this << " and quit backend " << (void *)backend << __FILE__ << ":" << __LINE__ << std::endl;
    else
        std::cerr << "Http::detectTimeout() need to quit " << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
    //if no byte received into 600s (10m)
    parseNonHttpError(Backend::NonHttpError_Timeout);
    /*do into disconnectFrontend():
    for(Client * client : clientsList)
    {
        client->writeEnd();
        client->disconnect();
    }
    clientsList.clear();*/
    disconnectFrontend();
    if(backend!=nullptr)
    {
        //disconnectBackend();-> can't just connect the backend because the remaining data neeed to be consumed
        backend->close();//keep the backend running, clean close
    }
    else // was in pending list
    {
        disconnectBackend();
    }
    return true;
}

std::string Http::getQuery() const
{
    std::string ret;
    char buffer[32];
    std::snprintf(buffer,sizeof(buffer),"%p",(void *)this);
    ret+=std::string(buffer);
    if(!isAlive())
        ret+=" not alive";
    else
        ret+=" alive on "+getUrl();
    if(backend!=nullptr)
    {
        std::snprintf(buffer,sizeof(buffer),"%p",(void *)backend);
        ret+=" on backend "+std::string(buffer);
    }
    if(!clientsList.empty())
        ret+=" with "+std::to_string(clientsList.size())+" client(s)";
    ret+=" last byte "+std::to_string(lastReceivedBytesTimestamps);
    if(!etagBackend.empty())
        ret+=", etagBackend: "+etagBackend;
    if(requestSended)
        ret+=", requestSended";
    else
        ret+=", !requestSended";
    if(tempCache!=nullptr)
        ret+=", tempCache: "+cachePath;
    if(finalCache!=nullptr)
        ret+=", finalCache: "+cachePath;
    if(endDetected)
        ret+=", endDetected";
    else
        ret+=", !endDetected";
    return ret;
}
