#include "Client.hpp"
#include "Dns.hpp"
#include "Cache.hpp"
#include "Http.hpp"
#include "Https.hpp"
#include "Common.hpp"
#include <unistd.h>
#include <iostream>
#include <string.h>
//#include <xxhash.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "xxHash/xxh3.h"
#include <chrono>

//ETag -> If-None-Match

#ifdef DEBUGFASTCGI
#include <arpa/inet.h>
#endif

Client::Client(int cfd) :
    EpollObject(cfd,EpollObject::Kind::Kind_Client),
    fastcgiid(-1),
    readCache(nullptr),
    http(nullptr),
    fullyParsed(false),
    endTriggered(false),
    status(Status_Idle),
    https(false),
    partial(false),
    partialEndOfFileTrigged(false),
    outputWrited(false)
{
    Cache::newFD(cfd,this,EpollObject::Kind::Kind_Client);
    this->kind=EpollObject::Kind::Kind_Client;
    this->fd=cfd;
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " Client::Client() " << this << " fd: " << fd << " this->fd: " << this->fd << " constructor" << std::endl;
    #endif
    #ifdef MAXFILESIZE
    readSizeFromCache=0;
    #endif
}

Client::~Client()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " destructor " << this << std::endl;
    #endif
    #ifdef DEBUGFILEOPEN
    std::cerr << "Client::~Client(), readCache close: " << readCache << std::endl;
    #endif
    if(readCache!=nullptr)
    {
        readCache->close();
        delete readCache;
        readCache=nullptr;
    }
    if(http!=nullptr)
    {
        if(!http->removeClient(this))
        {
            #ifdef DEBUGFASTCGI
            std::cerr << this << " not into client list of " << http << __FILE__ << ":" << __LINE__ << std::endl;
            #endif
        }
        http=nullptr;
    }
    if(fd!=-1)
        Cache::closeFD(fd);
}

void Client::parseEvent(const epoll_event &event)
{
    #ifdef DEBUGFASTCGI
    std::cout << this << " Client event.events: " << event.events << std::endl;
    #endif
    if(event.events & EPOLLIN)
        readyToRead();
    if(event.events & EPOLLOUT)
        readyToWrite();
    if(event.events & EPOLLHUP)
        disconnect();
    if(event.events & EPOLLRDHUP)
        disconnect();
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << "event.events: " << event.events << " " << this << std::endl;
    #endif
}

void Client::disconnect()
{
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
            abort();
        }
    }
    #endif
    #ifdef DEBUGFASTCGI
    {
        struct stat sb;
        if(fstat(fd,&sb)!=0)
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " size: " << sb.st_size << std::endl;
        else
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    }
    #endif
    #ifdef DEBUGFILEOPEN
    std::cerr << "Client::disconnect(), readCache close: " << fd << std::endl;
    #endif
    if(fd!=-1)
    {
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
        }
        #endif
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << fd << " disconnect() close()" << std::endl;
        #endif
        Cache::closeFD(fd);
        epoll_ctl(epollfd,EPOLL_CTL_DEL, fd, NULL);
        if(::close(fd)!=0)
            std::cerr << this << " " << fd << " disconnect() failed: " << errno << std::endl;
        fd=-1;
    }
    disconnectFromHttp();
    dataToWrite.clear();
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
            abort();
        }
    }
    #endif
    if(status==Status_WaitDns)
        Dns::dns->cancelClient(this,host,https);
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
            abort();
        }
    }
    #endif
    fastcgiid=-1;
}

void Client::disconnectFromHttp()
{
    if(http!=nullptr)
    {
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
        }
        #endif
        if(!http->removeClient(this))
        {
            #ifdef DEBUGFASTCGI
            std::cerr << this << " not into client list of " << http << " " << __FILE__ << ":" << __LINE__ << std::endl;
            #endif
        }
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
        }
        #endif
        http=nullptr;
    }
}

void Client::readyToRead()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " fd: " << fd << " this->fd: " << this->fd << std::endl;
    #endif
    if(fullyParsed)
        return;
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif

    char buff[4096];
    const int size=read(fd,buff,sizeof(buff));
    if(size<=0)
        return;

    std::string ifNoneMatch;
    https=false;
    uri.clear();
    host.clear();
    //all is big endian
    int pos=0;
    uint8_t var8=0;
    uint16_t var16=0;

    /*{
        std::cerr << fd << ") " << Common::binarytoHexa(buff,size) << std::endl;
    }*/

    do
    {
        if(!read8Bits(var8,buff,size,pos))
        {
            std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
            return;
        }
        if(var8!=1)
        {
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
            #endif
            disconnect();
            return;
        }
        if(!read8Bits(var8,buff,size,pos))
        {
            std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
            return;
        }
        if(fastcgiid==-1)
        {
            if(var8!=1)
            {
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                #endif
                disconnect();
                return;
            }
            if(!read16Bits(var16,buff,size,pos))
            {
                std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
                return;
            }
            fastcgiid=var16;
        }
        else
        {
            if(var8!=4 && var8!=5)
            {
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                #endif
                disconnect();
                return;
            }
            if(!read16Bits(var16,buff,size,pos))
                return;
            if(fastcgiid!=var16)
            {
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                #endif
                disconnect();
                return;
            }
        }
        uint16_t contentLenght=0;
        uint8_t paddingLength=0;
        if(!read16Bits(contentLenght,buff,size,pos))
        {
            std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
            return;
        }
        if(!read8Bits(paddingLength,buff,size,pos))
        {
            std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
            return;
        }
        if(!canAddToPos(1,size,pos))
        {
            std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
            return;
        }
        switch (var8) {
        //FCGI_BEGIN_REQUEST
        case 1:
            //skip the content length + padding length
            if(!canAddToPos(contentLenght+paddingLength,size,pos))
            {
                std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
                return;
            }
        break;
        //FCGI_PARAMS
        case 4:
        {
            int contentLenghtAbs=contentLenght+pos;
            while(pos<contentLenghtAbs)
            {
                uint32_t varSize=0;
                uint8_t varSize8=0;
                if(!read8Bits(varSize8,buff,size,pos))
                {
                    std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
                    return;
                }
                if(varSize8>127)
                {
                    if(!read24Bits(varSize,buff,size,pos))
                    {
                        std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
                        return;
                    }
                }
                else
                    varSize=varSize8;

                uint32_t valSize=0;
                uint8_t valSize8=0;
                if(!read8Bits(valSize8,buff,size,pos))
                {
                    std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
                    return;
                }
                if(valSize8>127)
                {
                    if(!read24Bits(valSize,buff,size,pos))
                    {
                        std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
                        return;
                    }
                }
                else
                    valSize=valSize8;

                switch(varSize)
                {
                    //anti loop protection:
                    case 8:
                    if(memcmp(buff+pos,"EPNOERFT",8)==0 && valSize==8)
                        if(memcmp(buff+pos+varSize,"ysff43Uy",5)==0)
                        {
                            char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nAnti loop protection";
                            writeOutput(text,sizeof(text)-1);
                            writeEnd();
                            disconnect();
                            return;
                        }
                    break;
                    case 9:
                    if(memcmp(buff+pos,"HTTP_HOST",9)==0)
                        host=std::string(buff+pos+varSize,valSize);
                    break;
                    case 11:
                    if(memcmp(buff+pos,"REQUEST_URI",11)==0)
                        uri=std::string(buff+pos+varSize,valSize);
                    else if(memcmp(buff+pos,"REMOTE_ADDR",11)==0)
                    {
                        std::cout << "request from IP: " << std::string(buff+pos+varSize,valSize) << std::endl;
                    /* black list: self ip, block ip continuously downloading same thing
                        ifNoneMatch=std::string(buff+pos+varSize,8);
                        */
                    }
                    /*else if(memcmp(buff+pos,"SERVER_PORT",11)==0 && valSize==3)
                        if(memcmp(buff+pos+varSize,"443",3)==0)
                            https=true;*/
                    break;
                    case 14:
                    if(memcmp(buff+pos,"REQUEST_SCHEME",14)==0 && valSize==5)
                        if(memcmp(buff+pos+varSize,"https",5)==0)
                            https=true;
                    break;
                    case 18:
                    if(memcmp(buff+pos,"HTTP_IF_NONE_MATCH",18)==0 && valSize==8)
                        ifNoneMatch=std::string(buff+pos+varSize,8);
                    break;
                    default:
                    break;
                }
                //std::cout << std::string(buff+pos,varSize) << ": " << std::string(buff+pos+varSize,valSize) << std::endl;
                if(!canAddToPos(varSize+valSize,size,pos))
                {
                    std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
                    return;
                }
            }
            if(!canAddToPos(paddingLength,size,pos))
            {
                std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
                return;
            }
        }
        break;
        //FCGI_STDIN
        case 5:
            //skip the content length + padding length
            if(!canAddToPos(contentLenght+paddingLength,size,pos))
            {
                std::cerr << __FILE__ << ":" << __LINE__ << " FastCGI protocol error: " << Common::hexaToBinary(std::string(buff,size)) << " " << size << std::endl;
                return;
            }
            fullyParsed=true;
        break;
        default:
            break;
        }
    } while(pos<size);

    if(!fullyParsed)
        return;
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
    #endif

    //check if robots.txt
    if(uri=="/robots.txt")
    {
        char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nUser-agent: *\r\nDisallow: /";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        disconnect();
        return;
    }
    //check if robots.txt
    if(uri=="/favicon.ico")
    {
        char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nDropped for now";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        disconnect();
        return;
    }

    //resolv the host or from subdomain or from uri
    {
        //resolv final url (hex, https, ...)
        const size_t &pos=host.rfind(".confiared.com");
        const size_t &mark=(host.size()-14);
        #ifdef DEBUGDNS
        const size_t &posdebug=host.rfind(".bolivia-online.com");
        const size_t &markdebug=(host.size()-19);
        if(pos==mark || posdebug==markdebug)
        {
            std::string hostb;
            if(pos==mark)
                hostb=host.substr(0,mark);
            else
                hostb=host.substr(0,markdebug);
        #else
        if(pos==mark)
        {
            std::string hostb=host.substr(0,mark);
        #endif

            size_t posb=hostb.rfind("cdn");
            size_t markb=(hostb.size()-3);
            if(posb==markb)
            {
                if(markb>1)
                    host=Common::hexaToBinary(hostb.substr(0,markb-1));
                else if(markb==0)
                {
                    const size_t poss=uri.find("/",1);
                    if(poss!=std::string::npos)
                    {
                        if(poss>2)
                        {
                            host=uri.substr(1,poss-1);
                            uri=uri.substr(poss);
                        }
                    }
                    else
                    {
                        //std::cerr << "uri '/' not found " << uri << ", host: " << host << std::endl;
                        char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nCDN bad usage: contact@confiared.com";
                        writeOutput(text,sizeof(text)-1);
                        writeEnd();
                        disconnect();
                        return;
                    }
                }
            }
            else
            {
                markb=(hostb.size()-4);
                posb=hostb.rfind("cdn1");
                if(posb!=markb)
                    posb=hostb.rfind("cdn2");
                if(posb!=markb)
                    posb=hostb.rfind("cdn3");
                if(posb==markb)
                {
                    if(markb>1)
                        host=Common::hexaToBinary(hostb.substr(0,markb-1));
                    else if(markb==0)
                    {
                        const size_t poss=uri.find("/",1);
                        if(poss!=std::string::npos)
                        {
                            if(poss>2)
                            {
                                host=uri.substr(1,poss-1);
                                uri=uri.substr(poss);
                            }
                        }
                        else
                        {
                            //std::cerr << "uri '/' not found " << uri << ", host: " << host << std::endl;
                            char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nCDN bad usage: contact@confiared.com";
                            writeOutput(text,sizeof(text)-1);
                            writeEnd();
                            disconnect();
                            return;
                        }
                    }
                }
            }
        }
        else
        {
            const size_t poss=uri.find("/",1);
            if(poss!=std::string::npos)
            {
                if(poss>2)
                {
                    host=uri.substr(1,poss-1);
                    uri=uri.substr(poss);
                }
            }
            else
            {
                //std::cerr << "uri '/' not found " << uri << ", host: " << host << std::endl;
                char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nCDN bad usage: contact@confiared.com";
                writeOutput(text,sizeof(text)-1);
                writeEnd();
                disconnect();
                return;
            }
        }
    }
    Client::loadUrl(host,uri,ifNoneMatch);
}

void Client::loadUrl(std::string host,const std::string &uri,const std::string &ifNoneMatch)
{
    //if have request
    const auto p1 = std::chrono::system_clock::now();
    if(https)
        std::cout << std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count() << " downloading: https://" << host << uri << std::endl;
    else
        std::cout << std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count() << " downloading: http://" << host << uri << std::endl;

    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
    #endif

    if(host.empty())
    {
        char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nCDN bad usage: contact@confiared.com";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        disconnect();
        return;
    }

    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
    #endif
    status=Status_WaitTheContent;
    partial=false;
    std::string hostwithprotocol=host;
    if(https)
        hostwithprotocol+="s";

    std::string path("cache/");
    std::string folder;
    if(Cache::hostsubfolder)
    {
        const uint32_t &hashhost=static_cast<uint32_t>(XXH3_64bits(hostwithprotocol.data(),hostwithprotocol.size()));
        const XXH64_hash_t &hashuri=XXH3_64bits(uri.data(),uri.size());

        //do the hash for host to define cache subfolder, hash for uri to file

        //std::string folder;
        folder = Common::binarytoHexa(reinterpret_cast<const char *>(&hashhost),sizeof(hashhost));
        path+=folder+"/";

        const std::string urifolder = Common::binarytoHexa(reinterpret_cast<const char *>(&hashuri),sizeof(hashuri));
        path+=urifolder;
    }
    else
    {
        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        XXH3_64bits_update(&state, hostwithprotocol.data(),hostwithprotocol.size());
        XXH3_64bits_update(&state, uri.data(),uri.size());
        const XXH64_hash_t &hashuri=XXH3_64bits_digest(&state);

        const std::string urifolder = Common::binarytoHexa(reinterpret_cast<const char *>(&hashuri),sizeof(hashuri));
        path+=urifolder;
    }

/*    if(path=="cache/A66F8D1178084ED8" || path=="cache/A66F8D1178084ED8.tmp" || path=="cache/614A5ACA52C8092E" || path=="cache/614A5ACA52C8092E.tmp" || path=="cache/9F187A83C03CB2F2" || path=="cache/9F187A83C03CB2F2.tmp")
    {
        char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nBlacklisted file";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        disconnect();
        return;
    }*/
    bool httpBackendFound=false;
    if(!https)
    {
        if(Http::pathToHttp.find(path)!=Http::pathToHttp.cend())
        {
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
            #endif
            if(http!=nullptr)
            {
                if(!http->removeClient(this))
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << this << " not into client list of " << http << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                }
                http=nullptr;
            }
            if(!Http::pathToHttp.at(path)->isAlive())
            {
                std::cerr << this << " http " << path << " is not alive" << __FILE__ << ":" << __LINE__ << std::endl;
                Http *http=Http::pathToHttp.at(path);
                Http::pathToHttp.erase(path);
                http->disconnectBackend();
                http->disconnectFrontend();
                delete http;
            }
            else
            {
                httpBackendFound=true;
                http=Http::pathToHttp.at(path);
                http->addClient(this);//into this call, start open cache and stream if partial have started
            }
        }
    }
    else
    {
        if(Https::pathToHttps.find(path)!=Https::pathToHttps.cend())
        {
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
            #endif
            if(http!=nullptr)
            {
                if(!http->removeClient(this))
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << this << " not into client list of " << http << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                }
                http=nullptr;
            }
            if(!Https::pathToHttps.at(path)->isAlive())
            {
                std::cerr << this << " http " << path << " is not alive" << __FILE__ << ":" << __LINE__ << std::endl;
                Http *http=Https::pathToHttps.at(path);
                Https::pathToHttps.erase(path);
                http->disconnectBackend();
                http->disconnectFrontend();
                delete http;
            }
            else
            {
                httpBackendFound=true;
                http=Https::pathToHttps.at(path);
                http->addClient(this);//into this call, start open cache and stream if partial have started
            }
        }
    }
    if(!httpBackendFound)
    {
        struct stat sb;
        std::string url;
        if(https)
            url="https://";
        else
            url="http://";
        url+=host;
        url+=uri;
        //try open cache
        const bool cacheWasExists=stat(path.c_str(),&sb)==0;
        #ifdef DEBUGFASTCGI
        if(cacheWasExists)
        {std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " " << path.c_str() << " cache size: " << sb.st_size << std::endl;}
        #endif
        //std::cerr << "open((path).c_str() " << path << std::endl;
        int cachefd = open(path.c_str(), O_RDWR | O_NOCTTY/* | O_NONBLOCK*/, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        //if failed open cache
        if(cachefd==-1)
        {
            if(errno!=2)//if not file not found
                std::cerr << "can't open cache file " << path << " for " << url << " due to errno: " << errno << std::endl;
            if(Cache::hostsubfolder)
                ::mkdir(("cache/"+folder).c_str(),S_IRWXU);

            //get AAAA entry for host
            if(!Dns::dns->get(this,host,https))
            {
                char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nOverloaded CDN Dns";
                std::cerr << "Overloaded CDN Dns " << __FILE__ << ":" << __LINE__ << std::endl;
                writeOutput(text,sizeof(text)-1);
                writeEnd();
                disconnect();
                return;
            }
            status=Status_WaitDns;
            return;
        }
        else
        {
            #ifdef DEBUGFILEOPEN
            std::cerr << "Client::loadUrl() open: " << path << ", fd: " << cachefd << ", " << url << std::endl;
            #endif
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
            #endif
            if(!ifNoneMatch.empty())
            {
                char bufferETag[6];
                if(::pread(cachefd,bufferETag,sizeof(bufferETag),2*sizeof(uint64_t)+sizeof(uint16_t))==sizeof(bufferETag))
                {
                    if(memcmp(ifNoneMatch.substr(1,6).data(),bufferETag,sizeof(bufferETag))==0)
                    {
                        //frontend 304
                        char text[]="Status: 304 Not Modified\r\n\r\n";
                        writeOutput(text,sizeof(text)-1);
                        writeEnd();
                        disconnect();
                        #ifdef DEBUGFILEOPEN
                        std::cerr << "Client::loadUrl(), readCache close: " << cachefd << std::endl;
                        #endif
                        ::close(cachefd);
                        #ifdef DEBUGFASTCGI
                        std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
                        #endif
                        return;
                    }
                }
            }

            fstat(fd,&sb);
            #ifdef DEBUGFASTCGI
            {std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " cache size: " << sb.st_size << std::endl;}
            #endif
            if(sb.st_size<25)
            {
                ::close(cachefd);
                std::cerr << "corruption detected, new file? for " << path << " url: " << url << std::endl;

                //get AAAA entry for host
                if(!Dns::dns->get(this,host,https))
                {
                    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nOverloaded CDN Dns";
                    std::cerr << "Overloaded CDN Dns " << __FILE__ << ":" << __LINE__ << std::endl;
                    writeOutput(text,sizeof(text)-1);
                    writeEnd();
                    disconnect();
                    return;
                }
                status=Status_WaitDns;
                return;
            }
            uint64_t lastModificationTimeCheck=0;
            if(::pread(cachefd,&lastModificationTimeCheck,sizeof(lastModificationTimeCheck),1*sizeof(uint64_t))!=sizeof(lastModificationTimeCheck))
            {
                ::close(cachefd);
                std::cerr << "corruption detected, bug? for " << path << " url: " << url << std::endl;

                //get AAAA entry for host
                if(!Dns::dns->get(this,host,https))
                {
                    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nOverloaded CDN Dns";
                    std::cerr << "Overloaded CDN Dns " << __FILE__ << ":" << __LINE__ << std::endl;
                    writeOutput(text,sizeof(text)-1);
                    writeEnd();
                    disconnect();
                    return;
                }
                status=Status_WaitDns;
                return;

                lastModificationTimeCheck=0;
            }
            uint16_t http_code=500;
            if(::pread(cachefd,&http_code,sizeof(http_code),2*sizeof(uint64_t))!=sizeof(http_code))
            {
                ::close(cachefd);
                std::cerr << "corruption detected, bug? for " << path << " url: " << url << std::endl;

                //get AAAA entry for host
                if(!Dns::dns->get(this,host,https))
                {
                    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nOverloaded CDN Dns";
                    std::cerr << "Overloaded CDN Dns " << __FILE__ << ":" << __LINE__ << std::endl;
                    writeOutput(text,sizeof(text)-1);
                    writeEnd();
                    disconnect();
                    return;
                }
                status=Status_WaitDns;
                return;

                http_code=500;
            }
            //last modification time check <24h or in future to prevent time drift
            const uint64_t &currentTime=time(NULL);
            if(lastModificationTimeCheck>currentTime)
            {
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " lastModificationTimeCheck>currentTime, time drift?" << std::endl;
                #endif
                lastModificationTimeCheck=currentTime;
            }
            if(lastModificationTimeCheck>(currentTime-Cache::timeToCache(http_code)))
            {
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
                #endif
                if(readCache!=nullptr)
                {
                    delete readCache;
                    readCache=nullptr;
                }
                readCache=new Cache(cachefd);
                readCache->set_access_time(currentTime);
                if(startRead())
                    return;
                else//corrupted, then recreate
                {
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " " << path.c_str() << " corrupted, delete to recreate" << std::endl;
                    ::close(cachefd);
                    ::unlink(path.c_str());
                }
            }
            else
            {
                #ifdef DEBUGFILEOPEN
                std::cerr << "Client::loadUrl(), readCache close: " << cachefd << ", " << __FILE__ << ":" << __LINE__ << std::endl;
                #endif
                //without the next line descriptor lost, generate: errno 24 (Too many open files)
                ::close(cachefd);
            }

            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
            #endif
            if(Cache::hostsubfolder)
                ::mkdir(("cache/"+folder).c_str(),S_IRWXU);

            //get AAAA entry for host
            if(!Dns::dns->get(this,host,https))
            {
                char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nOverloaded CDN Dns";
                std::cerr << "Overloaded CDN Dns " << __FILE__ << ":" << __LINE__ << std::endl;
                writeOutput(text,sizeof(text)-1);
                writeEnd();
                disconnect();
                return;
            }
            status=Status_WaitDns;
            return;
        }
    }
}

bool Client::canAddToPos(const int &i, const int &size, int &pos)
{
    if((pos+i)>size)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
        #endif
        disconnect();
        return false;
    }
    pos+=i;
    return true;
}

bool Client::read8Bits(uint8_t &var, const char * const data, const int &size, int &pos)
{
    if((pos+(int)sizeof(var))>size)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
        #endif
        disconnect();
        return false;
    }
    var=data[pos];
    pos+=sizeof(var);
    return true;
}

bool Client::read16Bits(uint16_t &var, const char * const data, const int &size, int &pos)
{
    if((pos+(int)sizeof(var))>size)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
        #endif
        disconnect();
        return false;
    }
    uint16_t t;
    memcpy(&t,data+pos,sizeof(var));
    var=be16toh(t);
    pos+=sizeof(var);
    return true;
}

bool Client::read24Bits(uint32_t &var, const char * const data, const int &size, int &pos)
{
    if((pos+(int)sizeof(var)-1)>size)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
        #endif
        disconnect();
        return false;
    }
    uint32_t t=0;
    memcpy(reinterpret_cast<char *>(&t)+1,data+pos,sizeof(var)-1);
    var=be32toh(t);
    pos+=sizeof(var)-1;
    return true;
}

void Client::dnsError()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    status=Status_Idle;
    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nDns Error";
    writeOutput(text,sizeof(text)-1);
    writeEnd();
    disconnect();
}

void Client::dnsWrong()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    status=Status_Idle;
    char text[]="Status: 403 Forbidden\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nThis site DNS (AAAA entry) is not into Confiared IPv6 range";
    writeOutput(text,sizeof(text)-1);
    writeEnd();
    disconnect();
}

void Client::dnsRight(const sockaddr_in6 &sIPv6)
{
    /*
     * 2 list: oldest usage/size
     *
     * Try: Machine Learning Based Cache Algorithm
     * */

    #ifdef DEBUGFASTCGI
    char astring[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &(sIPv6.sin6_addr), astring, INET6_ADDRSTRLEN);
    if(std::string(astring)=="::")
    {
        std::cerr << "Internal error, try connect on ::" << std::endl;
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
        #endif
        status=Status_Idle;
        char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nTry connect on ::";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        disconnect();
        return;
    }
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    status=Status_WaitTheContent;
    partial=false;
    std::string hostwithprotocol=host;
    if(https)
        hostwithprotocol+="s";

    std::string path("cache/");
    std::string folder;
    if(Cache::hostsubfolder)
    {
        const uint32_t &hashhost=static_cast<uint32_t>(XXH3_64bits(hostwithprotocol.data(),hostwithprotocol.size()));
        const XXH64_hash_t &hashuri=XXH3_64bits(uri.data(),uri.size());

        //do the hash for host to define cache subfolder, hash for uri to file

        //std::string folder;
        folder = Common::binarytoHexa(reinterpret_cast<const char *>(&hashhost),sizeof(hashhost));
        path+=folder+"/";

        const std::string urifolder = Common::binarytoHexa(reinterpret_cast<const char *>(&hashuri),sizeof(hashuri));
        path+=urifolder;
    }
    else
    {
        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        XXH3_64bits_update(&state, hostwithprotocol.data(),hostwithprotocol.size());
        XXH3_64bits_update(&state, uri.data(),uri.size());
        const XXH64_hash_t &hashuri=XXH3_64bits_digest(&state);

        const std::string urifolder = Common::binarytoHexa(reinterpret_cast<const char *>(&hashuri),sizeof(hashuri));
        path+=urifolder;
    }

    bool httpBackendFound=false;
    if(!https)
    {
        if(Http::pathToHttp.find(path)!=Http::pathToHttp.cend())
        {
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
            #endif
            if(http!=nullptr)
            {
                if(!http->removeClient(this))
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << this << " not into client list of " << http << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                }
                http=nullptr;
            }
            if(!Http::pathToHttp.at(path)->isAlive())
            {
                Http *http=Http::pathToHttp.at(path);
                Http::pathToHttp.erase(path);
                http->disconnectBackend();
                http->disconnectFrontend();
                delete http;
            }
            else
            {
                httpBackendFound=true;
                http=Http::pathToHttp.at(path);
                http->addClient(this);//into this call, start open cache and stream if partial have started
            }
        }
    }
    else
    {
        if(Https::pathToHttps.find(path)!=Https::pathToHttps.cend())
        {
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
            #endif
            if(http!=nullptr)
            {
                if(!http->removeClient(this))
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << this << " not into client list of " << http << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                }
                http=nullptr;
            }
            if(!Https::pathToHttps.at(path)->isAlive())
            {
                Http *http=Https::pathToHttps.at(path);
                Https::pathToHttps.erase(path);
                http->disconnectBackend();
                http->disconnectFrontend();
                delete http;
            }
            else
            {
                httpBackendFound=true;
                http=Https::pathToHttps.at(path);
                http->addClient(this);//into this call, start open cache and stream if partial have started
            }
        }
    }
    if(!httpBackendFound)
    {
        std::string url;
        if(https)
            url="https://";
        else
            url="http://";
        url+=host;
        url+=uri;
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
        #endif
        #ifdef MAXFILESIZE
        {
            struct stat sb;
            if (stat(path.c_str(), &sb) == 0)
                std::cout << "delete before open " << path << " with size of " << sb.st_size << std::endl;
            if(Http::duplicateOpen.find(path)!=Http::duplicateOpen.cend())
            {
                std::cerr << "duplicate open same file (abort)" << std::endl;
                abort();
            }
        }
        #endif
        //try open cache
        //std::cerr << "open((path).c_str() " << path << std::endl;
        int cachefd = open(path.c_str(), O_RDWR | O_NOCTTY/* | O_NONBLOCK*/, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        //if failed open cache
        if(cachefd==-1)
        {
            cachefd=0;
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
            #endif
            if(errno!=2)//if not file not found
                std::cerr << "can't open cache file " << path << " for " << url << " due to errno: " << errno << std::endl;
            if(Cache::hostsubfolder)
                ::mkdir(("cache/"+folder).c_str(),S_IRWXU);

            if(https)
            {
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " fd: " << getFD() << std::endl;
                #endif
                Https *https=new Https(0, //0 if no old cache file found
                                      path);
                http=https;
                if(https->tryConnect(sIPv6,host,uri))
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " fd: " << getFD() << std::endl;
                    #endif
                }
                /*else
                {
                    //don't have backend, just wait -> tryConnect() call Backend::tryConnectInternalList()
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " Socket Error (1)" << std::endl;
                    #endif
                    status=Status_Idle;
                    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nSocket Error (1)";
                    writeOutput(text,sizeof(text)-1);
                    writeEnd();
                    disconnect();
                }*/
                if(Https::pathToHttps.find(path)==Https::pathToHttps.cend())
                    Https::pathToHttps[path]=https;
                else
                {
                    std::cerr << "Https::pathToHttps.find(" << path << ") already found, abort()" << std::endl;
                    abort();
                }
                https->addClient(this);//into this call, start open cache and stream if partial have started
            }
            else
            {
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " fd: " << getFD() << std::endl;
                #endif
                if(http!=nullptr)
                {
                    if(!http->removeClient(this))
                    {
                        #ifdef DEBUGFASTCGI
                        std::cerr << this << " not into client list of " << http << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                        #endif
                    }
                    http=nullptr;
                }
                http=new Http(0, //0 if no old cache file found
                                      path);
                if(http->tryConnect(sIPv6,host,uri))
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
                    #endif
                }
                else
                {
                    /*#ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                    #endif
                    status=Status_Idle;
                    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nSocket Error (1b)";
                    writeOutput(text,sizeof(text)-1);
                    writeEnd();
                    disconnect();*/
                }
                if(Http::pathToHttp.find(path)==Http::pathToHttp.cend())
                    Http::pathToHttp[path]=http;
                else
                {
                    std::cerr << "Http::pathToHttp.find(" << path << ") already found, abort()" << std::endl;
                    abort();
                }
                http->addClient(this);//into this call, start open cache and stream if partial have started
            }
        }
        else
        {
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
            #endif
            #ifdef DEBUGFILEOPEN
            std::cerr << "Client::dnsRight() open: " << path << ", fd: " << cachefd << std::endl;
            #endif
            uint64_t lastModificationTimeCheck=0;
            if(::pread(cachefd,&lastModificationTimeCheck,sizeof(lastModificationTimeCheck),1*sizeof(uint64_t))!=sizeof(lastModificationTimeCheck))
                lastModificationTimeCheck=0;
            uint16_t http_code=500;
            if(::pread(cachefd,&http_code,sizeof(http_code),2*sizeof(uint64_t))!=sizeof(http_code))
                http_code=500;
            //last modification time check <24h or in future to prevent time drift
            const uint64_t &currentTime=time(NULL);
            if(lastModificationTimeCheck>(currentTime-Cache::timeToCache(http_code)))
            {
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                #endif
                if(readCache!=nullptr)
                {
                    delete readCache;
                    readCache=nullptr;
                }
                readCache=new Cache(cachefd);
                readCache->set_access_time(currentTime);
                startRead();
                return;
            }
            if(Cache::hostsubfolder)
                ::mkdir(("cache/"+folder).c_str(),S_IRWXU);

            //get the ETag to compare with client
            std::string etag;
            {
                uint8_t etagBackendSize=0;
                if(::pread(cachefd,&etagBackendSize,sizeof(etagBackendSize),3*sizeof(uint64_t))==sizeof(etagBackendSize))
                {
                    char buffer[etagBackendSize];
                    if(::pread(cachefd,buffer,etagBackendSize,3*sizeof(uint64_t)+sizeof(uint8_t))==etagBackendSize)
                    {
                        etag=std::string(buffer,etagBackendSize);
                        #ifdef DEBUGFASTCGI
                        if(etag.find('\0')!=std::string::npos)
                        {
                            ::close(cachefd);
                            cachefd=0;
                            etag="etag contain \\0 abort";
                            #ifdef DEBUGFASTCGI
                            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                            #endif
                            status=Status_Idle;
                            char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nInternal error etag 0";
                            writeOutput(text,sizeof(text)-1);
                            writeEnd();
                            disconnect();
                            return;
                        }
                        #endif
                    }
                }
            }
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
            #endif

            if(https)
            {
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << " cachefd: " << cachefd << " " << this << std::endl;
                #endif
                Https *https=new Https(cachefd, //0 if no old cache file found
                                      path);
                http=https;
                if(https->tryConnect(sIPv6,host,uri,etag))
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                    #endif
                }
                else
                {
/*                    //don't have backend, just wait -> tryConnect() call Backend::tryConnectInternalList()
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " Socket Error (2)" << std::endl;
                    #endif
                    status=Status_Idle;
                    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nSocket Error (2)";
                    writeOutput(text,sizeof(text)-1);
                    writeEnd();
                    disconnect();*/
                }
                if(Https::pathToHttps.find(path)==Https::pathToHttps.cend())
                    Https::pathToHttps[path]=https;
                else
                {
                    std::cerr << "Https::pathToHttps.find(" << path << ") already found, abort()" << std::endl;
                    abort();
                }
                https->addClient(this);//into this call, start open cache and stream if partial have started
            }
            else
            {
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << " cachefd: " << cachefd << " " << this << std::endl;
                #endif
                if(http!=nullptr)
                {
                    if(!http->removeClient(this))
                    {
                        #ifdef DEBUGFASTCGI
                        std::cerr << this << " not into client list of " << http << __FILE__ << ":" << __LINE__ << std::endl;
                        #endif
                    }
                    http=nullptr;
                }
                http=new Http(cachefd, //0 if no old cache file found
                                      path);
                if(http->tryConnect(sIPv6,host,uri,etag))
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << "http->tryConnect() ok" << std::endl;
                    #endif
                }
                else
                {
/*                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                    #endif
                    status=Status_Idle;
                    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nSocket Error (2b)";
                    writeOutput(text,sizeof(text)-1);
                    writeEnd();
                    disconnect();*/
                }
                if(Http::pathToHttp.find(path)==Http::pathToHttp.cend())
                    Http::pathToHttp[path]=http;
                else
                {
                    std::cerr << "Http::pathToHttp.find(" << path << ") already found, abort()" << std::endl;
                    abort();
                }
                http->addClient(this);//into this call, start open cache and stream if partial have started
            }
        }
    }
}

bool Client::startRead()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << " Client::startRead()" << std::endl;
    #endif
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    if(!readCache->seekToContentPos())
    {
        std::cerr << __FILE__ << ":" << __LINE__ << " Client::startRead(): !readCache->seekToContentPos(), cache corrupted?" << std::endl;
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        status=Status_Idle;
        char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nUnable to read cache (1)";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        disconnect();
        return false;
    }
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    readCache->setAsync();
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    if(!readCache->seekToContentPos())
    {
        std::cerr << __FILE__ << ":" << __LINE__ << " Client::startRead(): !readCache->seekToContentPos(), cache corrupted bis?" << std::endl;
        return false;
    }
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    #ifdef MAXFILESIZE
    /*{
        ssize_t filepos=lseek(fd, 0L, SEEK_CUR );
        std::cerr << this << " filepos " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " filepos: " << filepos << ", errno: " << errno << std::endl;
    }*/
    #endif
    continueRead();
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    return true;
}

bool Client::startRead(const std::string &path, const bool &partial)
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
    #endif
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    this->partial=partial;
    //O_WRONLY -> failed, need Read too call Cache::seekToContentPos(), read used to get pos
    int cachefd = open(path.c_str(), O_RDWR | O_NOCTTY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    //if failed open cache
    if(cachefd==-1)
    {
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        status=Status_Idle;
        char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nUnable to read cache (2)";
        writeOutput(text,sizeof(text)-1);
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        writeEnd();
        disconnect();
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        return false;
    }
    else
    {
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        #ifdef DEBUGFILEOPEN
        std::cerr << "Client::startRead() open: " << path << ", fd: " << cachefd << std::endl;
        #endif
    }
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    const off_t &s=lseek(cachefd,1*sizeof(uint64_t),SEEK_SET);
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    if(s==-1)
    {
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " unable to seek" << std::endl;
        status=Status_Idle;
        char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nUnable to seek";
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        writeOutput(text,sizeof(text)-1);
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        writeEnd();
        disconnect();
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        return false;
    }
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
    #endif
    const uint64_t &currentTime=time(NULL);
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    if(readCache!=nullptr)
    {
        delete readCache;
        readCache=nullptr;
    }
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    readCache=new Cache(cachefd);
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    readCache->set_access_time(currentTime);
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    if(!readCache->seekToContentPos())
    {
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " unable to seek to content" << std::endl;
        status=Status_Idle;
        char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nUnable to seek to content";
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        writeOutput(text,sizeof(text)-1);
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        writeEnd();
        disconnect();
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        return false;
    }
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    return startRead();
}

void Client::tryResumeReadAfterEndOfFile()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << " Client::tryResumeReadAfterEndOfFile()" << std::endl;
    #endif
    if(partialEndOfFileTrigged)
        continueRead();
    else
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << " Client::tryResumeReadAfterEndOfFile() workaround" << std::endl;
        #endif
        continueRead();//workaround
    }
}

void Client::continueRead()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << " http " << http << std::endl;
    #endif
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    if(readCache==nullptr)
        return;
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    if(!dataToWrite.empty())
        return;
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    #ifdef MAXFILESIZE
    {
        ssize_t filepos=lseek(fd, 0L, SEEK_CUR );
        if(filepos>99000000)
            std::cerr << this << " filepos " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " filepos: " << filepos << std::endl;
    }
    #else
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
        #endif
    #endif
    char buffer[65536-1000];
    do {
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        #ifdef MAXFILESIZE
        {
            ssize_t filepos=lseek(fd, 0L, SEEK_CUR );
            if(filepos>99000000)
                std::cerr << this << " filepos " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " filepos: " << filepos << std::endl;
        }
        #endif
        const ssize_t &s=readCache->read(buffer,sizeof(buffer));
        #ifdef MAXFILESIZE
        if(s>0)
            readSizeFromCache+=s;
        if(readSizeFromCache>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big read size (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
        #endif
        if(s<1)
        {
            if(!partial)
            {
                #ifdef MAXFILESIZE
                if(http!=nullptr)
                {
                    struct stat sb;
                    int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
                    if(rstat==0 && sb.st_size>100000000)
                    {
                        std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                        abort();
                    }
                }
                #endif
                writeEnd();
                disconnect();
                #ifdef MAXFILESIZE
                if(http!=nullptr)
                {
                    struct stat sb;
                    int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
                    if(rstat==0 && sb.st_size>100000000)
                    {
                        std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                        abort();
                    }
                }
                #endif
            }
            else
            {
                #ifdef MAXFILESIZE
                if(http!=nullptr)
                {
                    struct stat sb;
                    int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
                    if(rstat==0 && sb.st_size>100000000)
                    {
                        std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                        abort();
                    }
                }
                #endif
                partialEndOfFileTrigged=true;
                //std::cout << "End of file, wait more" << std::endl;
            }
            #ifdef MAXFILESIZE
            if(http!=nullptr)
            {
                struct stat sb;
                int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
                if(rstat==0 && sb.st_size>100000000)
                {
                    std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                    abort();
                }
            }
            #endif
            return;
        }
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        partialEndOfFileTrigged=false;
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        #ifdef MAXFILESIZE
        {
            ssize_t filepos=lseek(fd, 0L, SEEK_CUR );
            if(filepos>99000000)
                std::cerr << this << " filepos " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " filepos: " << filepos << std::endl;
        }
        #endif
        writeOutput(buffer,s);
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        //if can't write all
        if(!dataToWrite.empty())
        {
            #ifdef MAXFILESIZE
            if(http!=nullptr)
            {
                struct stat sb;
                int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
                if(rstat==0 && sb.st_size>100000000)
                {
                    std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                    abort();
                }
            }
            #endif
            return;
        }
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        //if can write all, try again
    } while(1);
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
}

void Client::cacheError()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
    #endif
    status=Status_Idle;
    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nCache file error";
    writeOutput(text,sizeof(text)-1);
    writeEnd();
    disconnect();
}

void Client::readyToWrite()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
    #endif
    if(!dataToWrite.empty())
    {
        const ssize_t writedSize=::write(fd,dataToWrite.data(),dataToWrite.size());
        #ifdef MAXFILESIZE
        {
            struct stat sb;
            int rstat=fstat(fd,&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << "too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << std::endl;
                abort();
            }
        }
        #endif
        if(errno!=0 && errno!=EAGAIN)
        {
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
            #endif
            disconnect();
            return;
        }
        if(writedSize>=0)
            if((size_t)writedSize==dataToWrite.size())
                //event to continue to read file
                return;
        dataToWrite.erase(0,writedSize);

        if(endTriggered==true)
        {
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
            #endif
            endTriggered=false;

            #ifdef DEBUGFILEOPEN
            std::cerr << "Client::~Client(), readCache close: " << readCache << std::endl;
            #endif
            if(readCache!=nullptr)
            {
                readCache->close();
                delete readCache;
                readCache=nullptr;
            }

            disconnect();
        }

        return;
    }
    continueRead();
}

void Client::writeOutput(const char * const data,const int &size)
{
    #ifdef DEBUGFASTCGI
    //std::cerr << __FILE__ << ":" << __LINE__ << ", outputWrited: " << outputWrited << " content: " << Common::binarytoHexa(data,size) << std::endl;
    #endif
    outputWrited=true;
    uint16_t padding=0;//size-size%16;
    uint16_t paddingbe=htobe16(padding);

    char header[1+1+2+2+2];
    header[0]=1;
    //FCGI_STDOUT
    header[1]=6;
    uint16_t idbe=htobe16(fastcgiid);
    memcpy(header+1+1,&idbe,2);
    uint16_t sizebe=htobe16(size);
    memcpy(header+1+1+2,&sizebe,2);
    memcpy(header+1+1+2+2,&paddingbe,2);
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    write(header,1+1+2+2+2);
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    write(data,size);
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif

    if(padding>0)
    {
        char t[padding];
        write(t,padding);
    }
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
}

void Client::write(const char * const data,const int &size)
{
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    if(fd==-1)
        return;
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    if(data==nullptr)
        return;
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    if(!dataToWrite.empty())
    {
        dataToWrite+=std::string(data,size);
        return;
    }
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    /*{
        std::cerr << fd << " write) ";
        if(size>255)
            std::cerr << "size: " << size;
        else
        {
            for(int i=0;i<size;++i)
            {
                const unsigned char c = data[i];
                std::cerr << lut[c >> 4];
                std::cerr << lut[c & 15];
            }
        }
        std::cerr << std::endl;
    }*/

    errno=0;
    #ifdef MAXFILESIZE
    ssize_t filepos=lseek(fd, 0L, SEEK_CUR );
    ssize_t filesize=-1;
    {
        struct stat sb;
        int rstat=fstat(fd,&sb);
        if(rstat==0)
            filesize=sb.st_size>100000000;
    }
    #endif
    const int writedSize=::write(fd,data,size);
    #ifdef MAXFILESIZE
    {
        struct stat sb;
        int rstat=fstat(fd,&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << this << " too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " filepos: " << filepos << " filesize " << filesize << std::endl;
            abort();
        }
    }
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
    if(writedSize==size)
        return;
    else if(errno!=0 && errno!=EAGAIN)
    {
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
        #endif
        if(errno!=32)//if not BROKEN PIPE
            std::cerr << fd << ") error to write: " << errno << std::endl;
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        disconnect();
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        return;
    }
    if(errno==EAGAIN)
    {
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        dataToWrite+=std::string(data+writedSize,size-writedSize);
        return;
    }
    else
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " errno " << errno << " this " << this << " size " << size << " writedSize " << writedSize << std::endl;
        #endif
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        disconnect();
        #ifdef MAXFILESIZE
        if(http!=nullptr)
        {
            struct stat sb;
            int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
            if(rstat==0 && sb.st_size>100000000)
            {
                std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
                abort();
            }
        }
        #endif
        return;
    }
    #ifdef MAXFILESIZE
    if(http!=nullptr)
    {
        struct stat sb;
        int rstat=stat((http->cachePath+".tmp").c_str(),&sb);
        if(rstat==0 && sb.st_size>100000000)
        {
            std::cerr << (http->cachePath+".tmp") << " is too big (abort) " << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << std::endl;
            abort();
        }
    }
    #endif
}

void Client::httpError(const std::string &errorString)
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " fd: " << fd << " this->fd: " << this->fd << " " << this << std::endl;
    #endif
    const std::string &fullContent=
            "Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nError: "+
            errorString;
    writeOutput(fullContent.data(),fullContent.size());
    writeEnd();
    disconnect();
}

void Client::writeEnd()
{
    #ifdef DEBUGFASTCGI
    const auto p1 = std::chrono::system_clock::now();
    std::cerr << std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count() << " Client::writeEnd() ";
    if(http!=nullptr)
        std::cerr << http->getUrl() << " ";
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    disconnectFromHttp();
    if(!outputWrited)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " !outputWrited" << std::endl;
        #endif
        return;
    }
    if(partial && readCache!=nullptr)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << " !outputWrited" << std::endl;
        #endif
        continueRead();
    }
    char header[1+1+2+2+2+4+4];
    header[0]=1;
    //FCGI_END_REQUEST
    header[1]=3;
    uint16_t idbe=htobe16(fastcgiid);
    memcpy(header+1+1,&idbe,2);
    uint16_t sizebe=htobe16(8);
    memcpy(header+1+1+2,&sizebe,2);
    uint16_t padding=0;
    memcpy(header+1+1+2+2,&padding,2);
    uint32_t applicationStatus=0;
    memcpy(header+1+1+2+2+2,&applicationStatus,4);
    uint32_t protocolStatus=0;
    memcpy(header+1+1+2+2+2+4,&protocolStatus,4);

    if(!dataToWrite.empty())
    {
        dataToWrite+=std::string(header,sizeof(header));
        endTriggered=true;
        #ifdef DEBUGFILEOPEN
        std::cerr << "Client::writeEnd() pre, readCache close: " << readCache << std::endl;
        #endif
        return;
    }
    #ifdef DEBUGFILEOPEN
    std::cerr << "Client::writeEnd() post, readCache close: " << readCache << std::endl;
    #endif
    if(readCache!=nullptr)
    {
        readCache->close();
        delete readCache;
        readCache=nullptr;
    }

    write(header,sizeof(header));

    fastcgiid=-1;
    if(dataToWrite.empty())
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
        #endif
        disconnect();
    }
    else
        endTriggered=true;
}
