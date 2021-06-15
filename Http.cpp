#include "Http.hpp"
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
#include <sys/time.h>
#endif

#ifdef MAXFILESIZE
std::unordered_map<std::string,Http *> Http::duplicateOpen;
#endif

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
    requestSended(false),
    headerWriten(false),
    backend(nullptr),
    contentLengthPos(-1),
    chunkLength(-1)
{
    endDetected=false;
    lastReceivedBytesTimestamps=Backend::currentTime();
    #ifdef DEBUGFASTCGI
    std::cerr << "contructor " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
    if(cachePath.empty())
        std::cerr << "critical error cachePath.empty() " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    if(cachefd<=0)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << "Http::Http()cachefd==0 then tempCache(nullptr): " << this << std::endl;
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
    std::cerr << "destructor " << this << "uri: " << uri<< ": " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    std::cerr << "Http::~Http(): " << this << std::endl;
    delete tempCache;
    tempCache=nullptr;
    disconnectFrontend();
    disconnectBackend();
    for(Client * client : clientsList)
    {
        client->writeEnd();
        client->disconnect();
    }
    clientsList.clear();
}

bool Http::tryConnect(const sockaddr_in6 &s,const std::string &host,const std::string &uri,const std::string &etag)
{
    #ifdef DEBUGFASTCGI
    const auto p1 = std::chrono::system_clock::now();
    std::cerr << std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count() << " try connect " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
    if(etag.find('\0')!=std::string::npos)
        std::cerr << "etag contain \\0 abort" << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    this->host=host;
    this->uri=uri;
    this->etagBackend=etag;
    return tryConnectInternal(s);
}

bool Http::tryConnectInternal(const sockaddr_in6 &s)
{
    bool connectInternal=false;
    backend=Backend::tryConnectHttp(s,this,connectInternal);
    if(backend==nullptr)
    {
        const auto p1 = std::chrono::system_clock::now();
        std::cerr << std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count() << " " << this << ": unable to get backend for " << host << uri << std::endl;
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
    #ifdef DEBUGFASTCGI

    struct timeval tv;
    #ifdef DEBUGFASTCGI
    gettimeofday(&tv,NULL);
    std::cerr << "[" << tv.tv_sec << "] ";
    std::cerr << "Http::sendRequest() " << this << " " << __FILE__ << ":" << __LINE__ << " uri: " << uri << std::endl;
    #endif
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
        std::string h(std::string("GET ")+uri+" HTTP/1.1\nHOST: "+host+"\nEPNOERFT: ysff43Uy\n"
              #ifdef HTTPGZIP
              "Accept-Encoding: gzip\n"+
              #endif
                      "\n");
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
        //std::cerr << h << std::endl;
        #endif
        if(!socketWrite(h.data(),h.size()))
            std::cerr << "ERROR to write: " << h << " errno: " << errno << std::endl;
    }
    else
    {
        std::string h(std::string("GET ")+uri+" HTTP/1.1\nHOST: "+host+"\nEPNOERFT: ysff43Uy\nIf-None-Match: "+etagBackend+"\n"
              #ifdef HTTPGZIP
              "Accept-Encoding: gzip\n"+
              #endif
                      "\n");
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
    #ifdef MAXFILESIZE
    if(finalCache!=nullptr)
    {
        struct stat sb;
        int rstat=fstat(finalCache->getFD(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__;
            std::cerr << " finalCache fd: " << finalCache->getFD();
            std::cerr << std::endl;
            abort();
        }
    }
    if(tempCache!=nullptr)
    {
        struct stat sb;
        int rstat=fstat(tempCache->getFD(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__;
            std::cerr << " tempCache fd: " << tempCache->getFD();
            std::cerr << std::endl;
            abort();
        }
    }
    {
        struct stat sb;
        int rstat=stat((cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__;
            if(finalCache!=nullptr)
                std::cerr << " finalCache fd: " << finalCache->getFD();
            if(tempCache!=nullptr)
                std::cerr << " tempCache fd: " << tempCache->getFD();
            std::cerr << std::endl;
            abort();
        }
    }
    #endif

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
        #ifdef MAXFILESIZE
        {
            struct stat sb;
            int rstat=stat((cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
        }
        #endif
        errno=0;
        const ssize_t size=socketRead(buffer+offset,sizeof(buffer)-offset);
        #ifdef MAXFILESIZE
        {
            struct stat sb;
            int rstat=stat((cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
        }
        #endif
        readSize=size;
        #ifdef DEBUGFASTCGI
        std::cout << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
        if(size>0)
        {
            #ifdef MAXFILESIZE
            {
                struct stat sb;
                int rstat=stat((cachePath+".tmp").c_str(),&sb);
                if(rstat==0 && sb.st_size>100000000)
                {
                    std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                    abort();
                }
            }
            #endif
            lastReceivedBytesTimestamps=Backend::currentTime();
            //std::cout << std::string(buffer,size) << std::endl;
            if(parsing==Parsing_Content)
            {
                #ifdef MAXFILESIZE
                {
                    struct stat sb;
                    const int rstat=stat((cachePath+".tmp").c_str(),&sb);
                    if(rstat==0 && sb.st_size>100000000)
                    {
                        std::cerr << (cachePath+".tmp") << " is too big (abort)" << std::endl;
                        abort();
                    }
                }
                #endif
                write(buffer,size);
                #ifdef MAXFILESIZE
                {
                    struct stat sb;
                    const int rstat=stat((cachePath+".tmp").c_str(),&sb);
                    if(rstat==0 && sb.st_size>100000000)
                    {
                        std::cerr << (cachePath+".tmp") << " is too big (abort)" << std::endl;
                        abort();
                    }
                }
                #endif
                if(endDetected)
                    return;
                #ifdef MAXFILESIZE
                {
                    struct stat sb;
                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                    if(rstat==0 && sb.st_size>100000000)
                    {
                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                        abort();
                    }
                }
                #endif
            }
            else
            {
                #ifdef MAXFILESIZE
                {
                    struct stat sb;
                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                    if(rstat==0 && sb.st_size>100000000)
                    {
                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                        abort();
                    }
                }
                #endif
                uint16_t pos=0;
                if(http_code==0)
                {
                    #ifdef MAXFILESIZE
                    {
                        struct stat sb;
                        int rstat=stat((cachePath+".tmp").c_str(),&sb);
                        if(rstat==0 && sb.st_size>100000000)
                        {
                            std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                            abort();
                        }
                    }
                    #endif
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
                                #ifdef MAXFILESIZE
                                {
                                    struct stat sb;
                                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                    if(rstat==0 && sb.st_size>100000000)
                                    {
                                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                        abort();
                                    }
                                }
                                #endif
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
                                    #ifdef MAXFILESIZE
                                    {
                                        struct stat sb;
                                        int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                        if(rstat==0 && sb.st_size>100000000)
                                        {
                                            std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                            abort();
                                        }
                                    }
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
                #ifdef MAXFILESIZE
                {
                    struct stat sb;
                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                    if(rstat==0 && sb.st_size>100000000)
                    {
                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                        abort();
                    }
                }
                #endif
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
                #ifdef MAXFILESIZE
                {
                    struct stat sb;
                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                    if(rstat==0 && sb.st_size>100000000)
                    {
                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                        abort();
                    }
                }
                #endif

                parsing=Parsing_HeaderVar;
                uint16_t pos2=pos;
                //content-length: 5000
                if(http_code!=0)
                {
                    #ifdef MAXFILESIZE
                    {
                        struct stat sb;
                        int rstat=stat((cachePath+".tmp").c_str(),&sb);
                        if(rstat==0 && sb.st_size>100000000)
                        {
                            std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                            abort();
                        }
                    }
                    #endif
                    while(pos<size)
                    {
                        #ifdef MAXFILESIZE
                        {
                            struct stat sb;
                            int rstat=stat((cachePath+".tmp").c_str(),&sb);
                            if(rstat==0 && sb.st_size>100000000)
                            {
                                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                abort();
                            }
                        }
                        #endif
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
                            #ifdef MAXFILESIZE
                            {
                                struct stat sb;
                                int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                if(rstat==0 && sb.st_size>100000000)
                                {
                                    std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                    abort();
                                }
                            }
                            #endif
                        }
                        else if(c=='\n' || c=='\r')
                        {
                            #ifdef MAXFILESIZE
                            {
                                struct stat sb;
                                int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                if(rstat==0 && sb.st_size>100000000)
                                {
                                    std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                    abort();
                                }
                            }
                            #endif
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
                                #ifdef MAXFILESIZE
                                {
                                    struct stat sb;
                                    if (stat((cachePath+".tmp").c_str(), &sb) == 0)
                                        std::cout << "delete before open " << (cachePath+".tmp") << " with size of " << sb.st_size << std::endl;
                                    if(Http::duplicateOpen.find(cachePath+".tmp")!=Http::duplicateOpen.cend())
                                    {
                                        std::cerr << "duplicate open same file (abort) " << this << " " << getUrl() << " vs " << Http::duplicateOpen.at(cachePath+".tmp")->getUrl() << " " << Http::duplicateOpen.at(cachePath+".tmp") << std::endl;
                                        //abort();
                                    }
                                }
                                #endif
                                ::unlink((cachePath+".tmp").c_str());
                                int cachefd = open((cachePath+".tmp").c_str(), O_RDWR | O_CREAT | O_TRUNC/* | O_NONBLOCK*/, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                                if(cachefd==-1)
                                {
                                    #ifdef MAXFILESIZE
                                    {
                                        struct stat sb;
                                        int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                        if(rstat==0 && sb.st_size>100000000)
                                        {
                                            std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                            abort();
                                        }
                                    }
                                    #endif
                                    if(errno==2)
                                    {
                                        ::mkdir("cache/",S_IRWXU);
                                        if(Cache::hostsubfolder)
                                        {
                                            const std::string::size_type &n=cachePath.rfind("/");
                                            const std::string basePath=cachePath.substr(0,n);
                                            mkdir(basePath.c_str(),S_IRWXU);
                                        }
                                        #ifdef MAXFILESIZE
                                        if(Http::duplicateOpen.find(cachePath+".tmp")!=Http::duplicateOpen.cend())
                                        {
                                            std::cerr << "duplicate open same file (abort)" << std::endl;
                                            abort();
                                        }
                                        #endif
                                        ::unlink((cachePath+".tmp").c_str());
                                        cachefd = open((cachePath+".tmp").c_str(), O_RDWR | O_CREAT | O_TRUNC/* | O_NONBLOCK*/, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                                        if(cachefd==-1)
                                        {
                                            #ifdef MAXFILESIZE
                                            {
                                                struct stat sb;
                                                int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                                if(rstat==0 && sb.st_size>100000000)
                                                {
                                                    std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                    abort();
                                                }
                                            }
                                            #endif
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
                                            #ifdef MAXFILESIZE
                                            {
                                                struct stat sb;
                                                int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                                if(rstat==0 && sb.st_size>100000000)
                                                {
                                                    std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                    abort();
                                                }
                                            }
                                            #endif
                                            return;
                                        }
                                        else
                                        {
                                            #ifdef MAXFILESIZE
                                            {
                                                unsigned int index=0;
                                                while(index<clientsList.size())
                                                {
                                                    if(clientsList.at(index)->getFD()==cachefd)
                                                        std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than cachefd " << cachefd << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                                    index++;
                                                }
                                            }
                                            #endif
                                            Cache::newFD(cachefd,this,EpollObject::Kind::Kind_Cache);
                                            #ifdef MAXFILESIZE
                                            {
                                                unsigned int index=0;
                                                while(index<clientsList.size())
                                                {
                                                    if(clientsList.at(index)->getFD()==cachefd)
                                                        std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than cachefd " << cachefd << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                                    index++;
                                                }
                                            }
                                            #endif
                                            #ifdef MAXFILESIZE
                                            {
                                                struct stat sb;
                                                int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                                if(rstat==0 && sb.st_size>100000000)
                                                {
                                                    std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                    abort();
                                                }
                                            }
                                            #endif
                                            #ifdef MAXFILESIZE
                                            if(Http::duplicateOpen.find(cachePath+".tmp")==Http::duplicateOpen.cend())
                                                Http::duplicateOpen[cachePath+".tmp"]=this;
                                            else
                                                std::cerr << "Http::duplicateOpen duplicate insert: " << cachePath+".tmp" << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                            std::cout << "open((cachePath+.tmp).c_str() " << (cachePath+".tmp") << " for " << host << uri << " with FD " << cachefd << std::endl;
                                            #endif
                                            #ifdef DEBUGFILEOPEN
                                            std::cerr << "Http::readyToRead() open: " << cachePath << ", fd: " << cachefd << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                            #endif
                                        }
                                        #ifdef MAXFILESIZE
                                        {
                                            struct stat sb;
                                            int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                            if(rstat==0 && sb.st_size>100000000)
                                            {
                                                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                abort();
                                            }
                                        }
                                        #endif
                                    }
                                    else
                                    {
                                        #ifdef MAXFILESIZE
                                        {
                                            struct stat sb;
                                            int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                            if(rstat==0 && sb.st_size>100000000)
                                            {
                                                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                abort();
                                            }
                                        }
                                        #endif
                                        #ifdef DEBUGFASTCGI
                                        std::cout << "open((cachePath+.tmp).c_str() failed " << (cachePath+".tmp") << " errno " << errno << std::endl;
                                        #endif
                                        //return internal error
                                        disconnectFrontend();
                                        #ifdef DEBUGFASTCGI
                                        std::cout << __FILE__ << ":" << __LINE__ << std::endl;
                                        #endif
                                        #ifdef MAXFILESIZE
                                        {
                                            struct stat sb;
                                            int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                            if(rstat==0 && sb.st_size>100000000)
                                            {
                                                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                abort();
                                            }
                                        }
                                        #endif
                                        return;
                                    }
                                }
                                else
                                {
                                    #ifdef MAXFILESIZE
                                    {
                                        unsigned int index=0;
                                        while(index<clientsList.size())
                                        {
                                            if(clientsList.at(index)->getFD()==cachefd)
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than cachefd " << cachefd << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                            index++;
                                        }
                                    }
                                    #endif
                                    Cache::newFD(cachefd,this,EpollObject::Kind::Kind_Cache);
                                    #ifdef MAXFILESIZE
                                    {
                                        unsigned int index=0;
                                        while(index<clientsList.size())
                                        {
                                            if(clientsList.at(index)->getFD()==cachefd)
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than cachefd " << cachefd << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                            index++;
                                        }
                                    }
                                    #endif
                                    #ifdef MAXFILESIZE
                                    {
                                        struct stat sb;
                                        int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                        if(rstat==0 && sb.st_size>100000000)
                                        {
                                            std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                            abort();
                                        }
                                    }
                                    #endif
                                    #ifdef MAXFILESIZE
                                    if(Http::duplicateOpen.find(cachePath+".tmp")==Http::duplicateOpen.cend())
                                        Http::duplicateOpen[cachePath+".tmp"]=this;
                                    else
                                        std::cerr << "Http::duplicateOpen duplicate insert: " << cachePath+".tmp" << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                    std::cout << "open((cachePath+.tmp).c_str() " << (cachePath+".tmp") << " for " << host << uri << " with FD " << cachefd << std::endl;
                                    #endif
                                    #ifdef DEBUGFILEOPEN
                                    std::cerr << "Http::readyToRead() open: " << (cachePath+".tmp") << ", fd: " << cachefd << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                    #endif
                                }

                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(clientsList.at(index)->getFD()==cachefd)
                                            std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than cachefd " << cachefd << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif
                                tempCache=new Cache(cachefd);
                                std::string r;
                                char randomIndex[6];
                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(tempCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        if(finalCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif
                                read(Http::fdRandom,randomIndex,sizeof(randomIndex));
                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(tempCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        if(finalCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif
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
                                #ifdef MAXFILESIZE
                                {
                                    struct stat sb;
                                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                    if(rstat==0 && sb.st_size>100000000)
                                    {
                                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                        abort();
                                    }
                                }
                                #endif
                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(tempCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        if(finalCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif

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
                                #ifdef MAXFILESIZE
                                {
                                    struct stat sb;
                                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                    if(rstat==0 && sb.st_size>100000000)
                                    {
                                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                        abort();
                                    }
                                }
                                #endif
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
                                #ifdef MAXFILESIZE
                                {
                                    struct stat sb;
                                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                    if(rstat==0 && sb.st_size>100000000)
                                    {
                                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                        abort();
                                    }
                                }
                                #endif
                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(tempCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        if(finalCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif
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
                                #ifdef MAXFILESIZE
                                {
                                    struct stat sb;
                                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                    if(rstat==0 && sb.st_size>100000000)
                                    {
                                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                        abort();
                                    }
                                }
                                #endif
                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(tempCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        if(finalCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif
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
                                #ifdef MAXFILESIZE
                                {
                                    struct stat sb;
                                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                    if(rstat==0 && sb.st_size>100000000)
                                    {
                                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                        abort();
                                    }
                                }
                                #endif
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
                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(tempCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        if(finalCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif
                                #ifdef MAXFILESIZE
                                {
                                    struct stat sb;
                                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                    if(rstat==0 && sb.st_size>100000000)
                                    {
                                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                        abort();
                                    }
                                }
                                #endif

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
                                #ifdef MAXFILESIZE
                                if(header.size()>65536)
                                {
                                    std::cerr << "Header creation too big (" << header.size() << "), abort to debug " << __FILE__ << ":" << __LINE__ << std::endl;
                                    abort();
                                }
                                #endif
                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(tempCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        if(finalCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif
                                if(contentsize>=0)
                                    header+="Content-Length: "+std::to_string(contentsize)+"\n";
                                /*else
                                    header+="Transfer-Encoding: chunked\n";*/
                                #ifdef MAXFILESIZE
                                if(header.size()>65536)
                                {
                                    std::cerr << "Header creation too big (" << header.size() << "), abort to debug " << __FILE__ << ":" << __LINE__ << std::endl;
                                    abort();
                                }
                                #endif
                                #ifdef HTTPGZIP
                                if(!contentEncoding.empty())
                                {
                                    header+="Content-Encoding: "+contentEncoding+"\n";
                                    contentEncoding.clear();
                                }
                                #endif
                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(tempCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        if(finalCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif
                                if(!contenttype.empty())
                                    header+="Content-Type: "+contenttype+"\n";
                                else
                                    header+="Content-Type: text/html\n";
                                #ifdef MAXFILESIZE
                                if(header.size()>65536)
                                {
                                    std::cerr << "Header creation too big (" << header.size() << "), abort to debug " << __FILE__ << ":" << __LINE__ << std::endl;
                                    abort();
                                }
                                #endif
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
                                #ifdef MAXFILESIZE
                                if(r.size()>4096)
                                {
                                    std::cerr << "Header ETag creation too big (" << r.size() << "), abort to debug " << __FILE__ << ":" << __LINE__ << std::endl;
                                    abort();
                                }
                                if(header.size()>65536)
                                {
                                    std::cerr << "Header creation too big (" << header.size() << "), abort to debug " << __FILE__ << ":" << __LINE__ << std::endl;
                                    abort();
                                }
                                #endif
                                #ifdef DEBUGFASTCGI
                                //std::cout << "header: " << header << std::endl;
                                #endif
                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(tempCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        if(finalCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif
                                header+="\n";
                                tempCache->seekToContentPos();
                                #ifdef MAXFILESIZE
                                if(header.size()>65536)
                                {
                                    std::cerr << "Header creation too big (" << header.size() << "), abort to debug " << __FILE__ << ":" << __LINE__ << std::endl;
                                    abort();
                                }
                                #endif
                                #ifdef MAXFILESIZE
                                struct stat sb;
                                const int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                if(rstat==0 && sb.st_size>100000000)
                                {
                                    std::cerr << (cachePath+".tmp") << " is too big (abort)" << std::endl;
                                    abort();
                                }
                                #endif
                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(tempCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        if(finalCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif
                                if(headerWriten)
                                {
                                    std::cerr << "headerWriten already to true, critical error (abort)" << std::endl;
                                    abort();
                                }
                                else
                                {
                                    #ifdef MAXFILESIZE
                                    {
                                        struct stat sb;
                                        int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                        if(rstat==0 && sb.st_size>100000000)
                                        {
                                            std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                            abort();
                                        }
                                    }
                                    #endif
                                    #ifdef MAXFILESIZE
                                    {
                                        unsigned int index=0;
                                        while(index<clientsList.size())
                                        {
                                            if(tempCache!=nullptr)
                                                if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                    std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                            if(finalCache!=nullptr)
                                                if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                    std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                            index++;
                                        }
                                    }
                                    #endif
                                    headerWriten=true;
                                    if(tempCache->write(header.data(),header.size())!=(ssize_t)header.size())
                                    {
                                        #ifdef MAXFILESIZE
                                        struct stat sb;
                                        const int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                        if(rstat==0 && sb.st_size>100000000)
                                        {
                                            std::cerr << (cachePath+".tmp") << " is too big (abort)" << std::endl;
                                            abort();
                                        }
                                        #endif
                                        #ifdef MAXFILESIZE
                                        if(Http::duplicateOpen.find(cachePath+".tmp")!=Http::duplicateOpen.cend())
                                            Http::duplicateOpen.erase(cachePath+".tmp");
                                        else
                                            std::cerr << "Http::duplicateOpen erase not found: " << cachePath+".tmp" << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        #endif
                                        std::cerr << "Header creation failed, abort to debug " << __FILE__ << ":" << __LINE__ << host << uri << " " << cachePath << std::endl;
                                        tempCache->close();
                                        delete tempCache;
                                        tempCache=nullptr;
                                        disconnectFrontend();
                                        disconnectBackend();
                                        for(Client * client : clientsList)
                                            client->writeEnd();
                                        clientsList.clear();
                                        #ifdef MAXFILESIZE
                                        {
                                            struct stat sb;
                                            int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                            if(rstat==0 && sb.st_size>100000000)
                                            {
                                                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                abort();
                                            }
                                        }
                                        #endif
                                    }
                                    else
                                    {
                                        #ifdef MAXFILESIZE
                                        {
                                            unsigned int index=0;
                                            while(index<clientsList.size())
                                            {
                                                if(tempCache!=nullptr)
                                                    if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                        std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                                if(finalCache!=nullptr)
                                                    if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                        std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                                index++;
                                            }
                                        }
                                        #endif
                                        #ifdef MAXFILESIZE
                                        {
                                            struct stat sb;
                                            int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                            if(rstat==0 && sb.st_size>100000000)
                                            {
                                                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                abort();
                                            }
                                        }
                                        #endif
                                        epoll_event event;
                                        memset(&event,0,sizeof(event));
                                        event.data.ptr = tempCache;
                                        event.events = EPOLLOUT | EPOLLRDHUP | EPOLLHUP | EPOLLRDHUP | EPOLLERR;
                                        //std::cerr << "EPOLL_CTL_ADD bis: " << cachefd << std::endl;

                                        //tempCache->setAsync(); -> to hard for now

                                        #ifdef MAXFILESIZE
                                        {
                                            struct stat sb;
                                            int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                            if(rstat==0 && sb.st_size>100000000)
                                            {
                                                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                abort();
                                            }
                                        }
                                        #endif
                                        #ifdef MAXFILESIZE
                                        {
                                            unsigned int index=0;
                                            while(index<clientsList.size())
                                            {
                                                if(tempCache!=nullptr)
                                                    if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                        std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                                if(finalCache!=nullptr)
                                                    if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                        std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                                index++;
                                            }
                                        }
                                        #endif
                                        for(Client * client : clientsList)
                                            client->startRead(cachePath+".tmp",true);
                                        #ifdef MAXFILESIZE
                                        {
                                            struct stat sb;
                                            int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                            if(rstat==0 && sb.st_size>100000000)
                                            {
                                                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                abort();
                                            }
                                        }
                                        #endif
                                    }
                                }
                                #ifdef MAXFILESIZE
                                {
                                    struct stat sb;
                                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                    if(rstat==0 && sb.st_size>100000000)
                                    {
                                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                        abort();
                                    }
                                }
                                #endif
                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(tempCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        if(finalCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif
                                break;
                            }
                            else
                            {
                                #ifdef MAXFILESIZE
                                {
                                    unsigned int index=0;
                                    while(index<clientsList.size())
                                    {
                                        if(tempCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        if(finalCache!=nullptr)
                                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                        index++;
                                    }
                                }
                                #endif
                                #ifdef MAXFILESIZE
                                {
                                    struct stat sb;
                                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                    if(rstat==0 && sb.st_size>100000000)
                                    {
                                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                        abort();
                                    }
                                }
                                #endif
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
                                        #ifdef MAXFILESIZE
                                        {
                                            struct stat sb;
                                            int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                            if(rstat==0 && sb.st_size>100000000)
                                            {
                                                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                abort();
                                            }
                                        }
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
                                #ifdef MAXFILESIZE
                                {
                                    struct stat sb;
                                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                    if(rstat==0 && sb.st_size>100000000)
                                    {
                                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                        abort();
                                    }
                                }
                                #endif
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
                            #ifdef MAXFILESIZE
                            {
                                unsigned int index=0;
                                while(index<clientsList.size())
                                {
                                    if(tempCache!=nullptr)
                                        if(clientsList.at(index)->getFD()==tempCache->getFD())
                                            std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                    if(finalCache!=nullptr)
                                        if(clientsList.at(index)->getFD()==finalCache->getFD())
                                            std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                    index++;
                                }
                            }
                            #endif
                            pos2=pos;
                            #ifdef MAXFILESIZE
                            {
                                struct stat sb;
                                int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                if(rstat==0 && sb.st_size>100000000)
                                {
                                    std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                    abort();
                                }
                            }
                            #endif
                        }
                        else
                        {
                            #ifdef MAXFILESIZE
                            {
                                unsigned int index=0;
                                while(index<clientsList.size())
                                {
                                    if(tempCache!=nullptr)
                                        if(clientsList.at(index)->getFD()==tempCache->getFD())
                                            std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                    if(finalCache!=nullptr)
                                        if(clientsList.at(index)->getFD()==finalCache->getFD())
                                            std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                    index++;
                                }
                            }
                            #endif
                            #ifdef MAXFILESIZE
                            {
                                struct stat sb;
                                int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                if(rstat==0 && sb.st_size>100000000)
                                {
                                    std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                    abort();
                                }
                            }
                            #endif
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
                        #ifdef MAXFILESIZE
                        {
                            struct stat sb;
                            int rstat=stat((cachePath+".tmp").c_str(),&sb);
                            if(rstat==0 && sb.st_size>100000000)
                            {
                                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                abort();
                            }
                        }
                        #endif
                        #ifdef MAXFILESIZE
                        {
                            unsigned int index=0;
                            while(index<clientsList.size())
                            {
                                if(tempCache!=nullptr)
                                    if(clientsList.at(index)->getFD()==tempCache->getFD())
                                        std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                if(finalCache!=nullptr)
                                    if(clientsList.at(index)->getFD()==finalCache->getFD())
                                        std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                                index++;
                            }
                        }
                        #endif
                    }
                    #ifdef MAXFILESIZE
                    {
                        unsigned int index=0;
                        while(index<clientsList.size())
                        {
                            if(tempCache!=nullptr)
                                if(clientsList.at(index)->getFD()==tempCache->getFD())
                                    std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                            if(finalCache!=nullptr)
                                if(clientsList.at(index)->getFD()==finalCache->getFD())
                                    std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                            index++;
                        }
                    }
                    #endif
                    #ifdef MAXFILESIZE
                    {
                        struct stat sb;
                        int rstat=stat((cachePath+".tmp").c_str(),&sb);
                        if(rstat==0 && sb.st_size>100000000)
                        {
                            std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                            abort();
                        }
                    }
                    #endif
                }
                #ifdef MAXFILESIZE
                {
                    unsigned int index=0;
                    while(index<clientsList.size())
                    {
                        if(tempCache!=nullptr)
                            if(clientsList.at(index)->getFD()==tempCache->getFD())
                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                        if(finalCache!=nullptr)
                            if(clientsList.at(index)->getFD()==finalCache->getFD())
                                std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                        index++;
                    }
                }
                #endif
                #ifdef DEBUGFASTCGI
                std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
                #endif
                #ifdef MAXFILESIZE
                {
                    struct stat sb;
                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                    if(rstat==0 && sb.st_size>100000000)
                    {
                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                        abort();
                    }
                }
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
                    #ifdef MAXFILESIZE
                    if(finalSize>sizeof(buffer))
                    {
                        std::cerr << "Content block too big, abort to debug " << __FILE__ << ":" << __LINE__ << std::endl;
                        abort();
                    }
                    #endif
                    const size_t rSize=write(buffer+pos,finalSize);
                    if(endDetected || rSize<=0 || rSize!=finalSize)
                        return;
                    #ifdef MAXFILESIZE
                    {
                        unsigned int index=0;
                        while(index<clientsList.size())
                        {
                            if(tempCache!=nullptr)
                                if(clientsList.at(index)->getFD()==tempCache->getFD())
                                    std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                            if(finalCache!=nullptr)
                                if(clientsList.at(index)->getFD()==finalCache->getFD())
                                    std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                            index++;
                        }
                    }
                    #endif
                }
                else
                {
                    #ifdef MAXFILESIZE
                    {
                        unsigned int index=0;
                        while(index<clientsList.size())
                        {
                            if(tempCache!=nullptr)
                                if(clientsList.at(index)->getFD()==tempCache->getFD())
                                    std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                            if(finalCache!=nullptr)
                                if(clientsList.at(index)->getFD()==finalCache->getFD())
                                    std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                            index++;
                        }
                    }
                    #endif
                    #ifdef MAXFILESIZE
                    {
                        struct stat sb;
                        int rstat=stat((cachePath+".tmp").c_str(),&sb);
                        if(rstat==0 && sb.st_size>100000000)
                        {
                            std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                            abort();
                        }
                    }
                    #endif
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
                                        #ifdef MAXFILESIZE
                                        {
                                            struct stat sb;
                                            int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                            if(rstat==0 && sb.st_size>100000000)
                                            {
                                                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                abort();
                                            }
                                        }
                                        #endif
                                        readyToRead();
                                        #ifdef MAXFILESIZE
                                        {
                                            struct stat sb;
                                            int rstat=stat((cachePath+".tmp").c_str(),&sb);
                                            if(rstat==0 && sb.st_size>100000000)
                                            {
                                                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                                                abort();
                                            }
                                        }
                                        #endif
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
                #ifdef MAXFILESIZE
                {
                    struct stat sb;
                    int rstat=stat((cachePath+".tmp").c_str(),&sb);
                    if(rstat==0 && sb.st_size>100000000)
                    {
                        std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                        abort();
                    }
                }
                #endif
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
            #ifdef MAXFILESIZE
            {
                unsigned int index=0;
                while(index<clientsList.size())
                {
                    if(tempCache!=nullptr)
                        if(clientsList.at(index)->getFD()==tempCache->getFD())
                            std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                    if(finalCache!=nullptr)
                        if(clientsList.at(index)->getFD()==finalCache->getFD())
                            std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                    index++;
                }
            }
            #endif
            #ifdef MAXFILESIZE
            {
                struct stat sb;
                int rstat=stat((cachePath+".tmp").c_str(),&sb);
                if(rstat==0 && sb.st_size>100000000)
                {
                    std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                    abort();
                }
            }
            #endif
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
        #ifdef MAXFILESIZE
        {
            unsigned int index=0;
            while(index<clientsList.size())
            {
                if(tempCache!=nullptr)
                    if(clientsList.at(index)->getFD()==tempCache->getFD())
                        std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than tempCache " << tempCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                if(finalCache!=nullptr)
                    if(clientsList.at(index)->getFD()==finalCache->getFD())
                        std::cerr << "Client " << clientsList.at(index) << ", have same FD: " << clientsList.at(index)->getFD() << " than finalCache " << finalCache->getFD() << " " << __FILE__ << ":" << __LINE__ << std::endl;
                index++;
            }
        }
        #endif
        #ifdef MAXFILESIZE
        {
            struct stat sb;
            int rstat=stat((cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
        }
        #endif
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
    } while(readSize>0);
    #ifdef MAXFILESIZE
    {
        struct stat sb;
        int rstat=stat((cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
            abort();
        }
    }
    #endif
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

std::unordered_map<std::string,Http *> &Http::pendingList()
{
    return Http::pathToHttp;
}

void Http::disconnectFrontend()
{
    #ifdef DEBUGFASTCGI
    std::cerr << "disconnectFrontend " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    #ifdef MAXFILESIZE
    {
        struct stat sb;
        int rstat=stat((cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (cachePath+".tmp") << " is too big (abort)" << std::endl;
            abort();
        }
    }
    #endif
    for(Client * client : clientsList)
    {
        client->writeEnd();
        client->disconnect();
    }
    clientsList.clear();
    #ifdef MAXFILESIZE
    {
        struct stat sb;
        int rstat=stat((cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (cachePath+".tmp") << " is too big (abort)" << std::endl;
            abort();
        }
    }
    #endif
    //disconnectSocket();

    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
    if(cachePath.empty())
        std::cerr << "cachePath.empty()" << std::endl;
    else
    {
        std::unordered_map<std::string,Http *> &pathToHttp=pendingList();
        if(pathToHttp.find(cachePath)==pathToHttp.cend())
            std::cerr << "Http::pathToHttp.find(" << cachePath << ")==Http::pathToHttp.cend()" << std::endl;
    }
    #endif
    if(!cachePath.empty())
    {
        std::unordered_map<std::string,Http *> &pathToHttp=pendingList();
        if(pathToHttp.find(cachePath)!=pathToHttp.cend())
            pathToHttp.erase(cachePath);
    }

    contenttype.clear();
    url.clear();
    headerBuff.clear();
    #ifdef MAXFILESIZE
    {
        struct stat sb;
        int rstat=stat((cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (cachePath+".tmp") << " is too big (abort)" << std::endl;
            abort();
        }
    }
    #endif
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
    clientsList.clear();
    if(!cachePath.empty())
    {
        std::unordered_map<std::string,Http *> &pathToHttp=pendingList();
        #ifdef DEBUGFASTCGI
        if(tempCache!=nullptr)
        {
            if(pathToHttp.find(cachePath+".tmp")==pathToHttp.cend())
            {
                std::cerr << "Http::backendError(" << errorString << "), but pathToHttp.find(" << cachePath+".tmp" << ") not found (abort) " << this << std::endl;
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
            else
                std::cerr << "Http::backendError(" << errorString << "), erase pathToHttp.find(" << cachePath << ") " << this << std::endl;
        }
        #endif
        if(finalCache!=nullptr)
            pathToHttp.erase(cachePath);
        if(tempCache!=nullptr)
            pathToHttp.erase(cachePath+".tmp");
        cachePath.clear();
    }
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

void Http::disconnectBackend()
{
    #ifdef DEBUGFASTCGI
    std::cerr << "Http::disconnectBackend() " << this << std::endl;
    #endif

    #ifdef DEBUGFILEOPEN
    std::cerr << "Http::disconnectBackend() post, finalCache close: " << finalCache << std::endl;
    #endif
    #ifdef MAXFILESIZE
    {
        struct stat sb;
        int rstat=stat((cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (cachePath+".tmp") << " is too big (abort)" << std::endl;
            abort();
        }
    }
    #endif
    if(finalCache!=nullptr)
        finalCache->close();
    const char * const cstr=cachePath.c_str();
    //todo, optimise with renameat2(RENAME_EXCHANGE) if --flatcache + destination
    #ifdef DEBUGFILEOPEN
    std::cerr << "Http::disconnectBackend() post, tempCache close: " << tempCache << std::endl;
    #endif
    #ifdef MAXFILESIZE
    {
        struct stat sb;
        int rstat=stat((cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (cachePath+".tmp") << " is too big (abort)" << std::endl;
            abort();
        }
    }
    #endif
    if(tempCache!=nullptr)
    {
        #ifdef MAXFILESIZE
        const auto p1 = std::chrono::system_clock::now();
        std::cout << std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count() << " close((cachePath+.tmp).c_str() " << (cachePath+".tmp") << " for " << host << uri << std::endl;
        if(Http::duplicateOpen.find(cachePath+".tmp")!=Http::duplicateOpen.cend())
            Http::duplicateOpen.erase(cachePath+".tmp");
        else
            std::cerr << "Http::duplicateOpen erase not found: " << cachePath+".tmp" << " " << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
        #ifdef MAXFILESIZE
        {
            struct stat sb;
            int rstat=stat((cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (cachePath+".tmp") << " is too big (abort)" << std::endl;
                abort();
            }
        }
        #endif
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
        #ifdef MAXFILESIZE
        {
            struct stat sb;
            int rstat=stat((cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (cachePath+".tmp") << " is too big (abort)" << std::endl;
                abort();
            }
        }
        #endif
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
                    #ifdef MAXFILESIZE
                    abort();
                    #else
                    ::unlink(cstr);
                    ::unlink((cachePath+".tmp").c_str());
                    #endif
                }
            }
            else
            {
                std::cerr << "Too big to be saved (abort): " << cachePath+".tmp" << " " << __FILE__ << ":" << __LINE__ << std::endl;
                #ifdef MAXFILESIZE
                abort();
                #else
                ::unlink(cstr);
                ::unlink((cachePath+".tmp").c_str());
                #endif
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
    if(backend!=nullptr)
        backend->downloadFinished();
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
                abort();
            }
    }
    for( const auto& n : Backend::addressToHttps )
    {
        const Backend::BackendList * list=n.second;
        for(const Backend * b : list->busy)
            if(b->http==this)
            {
                std::cerr << this << ": backend->http==this, backend https: " << backend << " " << getUrl() << " (abort)" << std::endl;
                abort();
            }
    }
    #endif
    backend=nullptr;

    #ifdef DEBUGFASTCGI
    std::cerr << "disconnectBackend " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    cachePath.clear();
    host.clear();
    uri.clear();
}

void Http::addClient(Client * client)
{
    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " add client: " << client << " client fd: " << client->getFD() << std::endl;
    if(cachePath.empty())
        std::cerr << "cachePath.empty()" << std::endl;
    else
    {
        std::unordered_map<std::string,Http *> &pathToHttp=pendingList();
        if(pathToHttp.find(cachePath)==pathToHttp.cend())
            std::cerr << "Http::pathToHttp.find(" << cachePath << ")==Http::pathToHttp.cend()" << std::endl;
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
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " backend " << backend << std::endl;
    #endif
    // can be without backend asigned due to max backend
}

bool Http::removeClient(Client * client)
{
    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " remove client: " << client << std::endl;
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
    //return false;
    #ifdef DEBUGFASTCGI
    if(itemDropped!=1)
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " remove client failed: " << client << ", itemDropped: " << itemDropped << std::endl;
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
        #ifdef MAXFILESIZE
        struct stat sb;
        int rstat=stat((cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (cachePath+".tmp") << " is too big (abort) 1" << std::endl;
            abort();
        }
        #endif
        const size_t &writedSize=tempCache->write((char *)data,size);
        #ifdef MAXFILESIZE
        rstat=stat((cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (cachePath+".tmp") << " is too big (abort) 2" << std::endl;
            abort();
        }
        #endif
        (void)writedSize;
        for(Client * client : clientsList)
            client->tryResumeReadAfterEndOfFile();
        #ifdef MAXFILESIZE
        rstat=stat((cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (cachePath+".tmp") << " is too big (abort) 3" << std::endl;
            abort();
        }
        #endif
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
            #ifdef MAXFILESIZE
            struct stat sb;
            rstat=stat((cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
            #endif
            disconnectFrontend();
            #ifdef MAXFILESIZE
            rstat=stat((cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (cachePath+".tmp") << " is too big (abort) 5" << std::endl;
                abort();
            }
            #endif
            disconnectBackend();
            #ifdef MAXFILESIZE
            rstat=stat((cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (cachePath+".tmp") << " is too big (abort) 6" << std::endl;
                abort();
            }
            #endif
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
    #ifdef MAXFILESIZE
    if(strnlen(buffer,sizeof(buffer))>4096)
    {
        std::cerr << "Header ETag creation too big (" << strnlen(buffer,sizeof(buffer)) << "), abort to debug " << __FILE__ << ":" << __LINE__ << std::endl;
        abort();
    }
    #endif
    return buffer;
}

//return true if timeout
bool Http::detectTimeout()
{
    const uint64_t msFrom1970=Backend::currentTime();
    if(lastReceivedBytesTimestamps>(msFrom1970-10*1000))
    {
        //prevent time drift
        if(lastReceivedBytesTimestamps>msFrom1970)
        {
            std::cerr << "Http::detectTimeout(), time drift" << std::endl;
            lastReceivedBytesTimestamps=msFrom1970;
        }
        return false;
    }
    //if no byte received into 600s (10m)
    parseNonHttpError(Backend::NonHttpError_Timeout);
    for(Client * client : clientsList)
    {
        client->writeEnd();
        client->disconnect();
    }
    clientsList.clear();
    if(backend!=nullptr)
    {
        disconnectBackend();
        //backend->close();//keep the backend running, another protection fix this
    }
    return true;
}

std::string Http::getQuery() const
{
    std::string ret;
    if(!isAlive())
        ret+="not alive";
    else
        ret+="alive on "+getUrl();
    if(!clientsList.empty())
        ret+=" with "+std::to_string(clientsList.size())+" client(s)";
    ret+=" last byte "+std::to_string(lastReceivedBytesTimestamps)+", etagBackend: "+etagBackend;
    if(requestSended)
        ret+=", requestSended";
    else
        ret+=", !requestSended";
    if(endDetected)
        ret+=", endDetected";
    else
        ret+=", !endDetected";
    return ret;
}
