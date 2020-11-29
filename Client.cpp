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

//ETag -> If-None-Match

#ifdef DEBUGFASTCGI
#include <arpa/inet.h>
#endif

Client::Client(int cfd) :
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
    this->kind=EpollObject::Kind::Kind_Client;
    this->fd=cfd;
    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << fd << " constructor" << std::endl;
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
}

void Client::parseEvent(const epoll_event &event)
{
    if(event.events & EPOLLIN)
        readyToRead();
    if(event.events & EPOLLOUT)
        readyToWrite();
    if(event.events & EPOLLHUP)
        disconnect();
}

void Client::disconnect()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    #ifdef DEBUGFILEOPEN
    std::cerr << "Client::disconnect(), readCache close: " << fd << std::endl;
    #endif
    if(fd!=-1)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << fd << " disconnect()" << std::endl;
        #endif
        //std::cerr << fd << " disconnect()" << std::endl;
        //std::cout << "disconnect()" << std::endl;
        epoll_ctl(epollfd,EPOLL_CTL_DEL, fd, NULL);
        if(::close(fd)!=0)
            std::cerr << this << " " << fd << " disconnect() failed: " << errno << std::endl;
        fd=-1;
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
    dataToWrite.clear();
    if(status==Status_WaitDns)
        Dns::dns->cancelClient(this,host,https);
    fastcgiid=-1;
}

void Client::readyToRead()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
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
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif

    //check if robots.txt
    if(uri=="/robots.txt")
    {
        char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nUser-agent: *\r\nDisallow: /";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        return;
    }
    //check if robots.txt
    if(uri=="/favicon.ico")
    {
        char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nDropped for now";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
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
                return;
            }
        }
    }
    Client::loadUrl(host,uri,ifNoneMatch);
}

void Client::loadUrl(std::string host,const std::string &uri,const std::string &ifNoneMatch)
{
    //if have request
/*    if(https)
        std::cout << "downloading: https://" << host << uri << std::endl;
    else
        std::cout << "downloading: http://" << host << uri << std::endl;*/

    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif

    if(host.empty())
    {
        char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nCDN bad usage: contact@confiared.com";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        return;
    }

    #ifdef DEBUGFASTCGI
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

    if(path=="cache/A66F8D1178084ED8" || path=="cache/A66F8D1178084ED8.tmp")
    {
        char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nBlacklisted file";
        #ifdef DEBUGFASTCGI
        std::cerr << this << " blacklisted file " << http << __FILE__ << ":" << __LINE__ << ": " << hostwithprotocol << uri << std::endl;
        #endif
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        return;
    }
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
        http=Http::pathToHttp.at(path);
        http->addClient(this);//into this call, start open cache and stream if partial have started
    }
    else
    {
        std::string url;
        if(https)
            url="https://";
        else
            url="http://";
        url+=host;
        url+=uri;
        //try open cache
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
                writeOutput(text,sizeof(text)-1);
                writeEnd();
                return;
            }
            status=Status_WaitDns;
        }
        else
        {
            #ifdef DEBUGFILEOPEN
            std::cerr << "Client::loadUrl() open: " << path << ", fd: " << cachefd << ", " << url << std::endl;
            #endif
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
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
                        #ifdef DEBUGFILEOPEN
                        std::cerr << "Client::loadUrl(), readCache close: " << cachefd << std::endl;
                        #endif
                        ::close(cachefd);
                        #ifdef DEBUGFASTCGI
                        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                        #endif
                        return;
                    }
                }
            }

            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
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
            else
            {
                #ifdef DEBUGFILEOPEN
                std::cerr << "Client::loadUrl(), readCache close: " << cachefd << ", " << __FILE__ << ":" << __LINE__ << std::endl;
                #endif
                //without the next line descriptor lost, generate: errno 24 (Too many open files)
                ::close(cachefd);
            }
            if(Cache::hostsubfolder)
                ::mkdir(("cache/"+folder).c_str(),S_IRWXU);

            //get AAAA entry for host
            if(!Dns::dns->get(this,host,https))
            {
                char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nOverloaded CDN Dns";
                writeOutput(text,sizeof(text)-1);
                writeEnd();
                return;
            }
            status=Status_WaitDns;
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
        abort();
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
        http=Http::pathToHttp.at(path);
        http->addClient(this);//into this call, start open cache and stream if partial have started
    }
    else
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
        //try open cache
        //std::cerr << "open((path).c_str() " << path << std::endl;
        int cachefd = open(path.c_str(), O_RDWR | O_NOCTTY/* | O_NONBLOCK*/, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        //if failed open cache
        if(cachefd==-1)
        {
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
                std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                #endif
                Https *https=new Https(0, //0 if no old cache file found
                                      path);
                http=https;
                if(https->tryConnect(sIPv6,host,uri))
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                    #endif
                    Https::pathToHttps[path]=https;
                    https->addClient(this);//into this call, start open cache and stream if partial have started
                }
                else
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                    #endif
                    status=Status_Idle;
                    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nSocket Error (1)";
                    writeOutput(text,sizeof(text)-1);
                    writeEnd();
                }
            }
            else
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
                http=new Http(0, //0 if no old cache file found
                                      path);
                if(http->tryConnect(sIPv6,host,uri))
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                    #endif
                    Http::pathToHttp[path]=http;
                    http->addClient(this);//into this call, start open cache and stream if partial have started
                }
                else
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                    #endif
                    status=Status_Idle;
                    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nSocket Error (1)";
                    writeOutput(text,sizeof(text)-1);
                    writeEnd();
                }
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

            std::string etag;
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
                        etag="etag contain \\0 abort";
                        abort();
                    }
                    #endif
                }
            }
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
            #endif

            if(https)
            {
                #ifdef DEBUGFASTCGI
                std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                #endif
                Https *https=new Https(cachefd, //0 if no old cache file found
                                      path);
                http=https;
                if(https->tryConnect(sIPv6,host,uri,etag))
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                    #endif
                    Https::pathToHttps[path]=https;
                    https->addClient(this);//into this call, start open cache and stream if partial have started
                }
                else
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                    #endif
                    status=Status_Idle;
                    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nSocket Error (2)";
                    writeOutput(text,sizeof(text)-1);
                    writeEnd();
                }
            }
            else
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
                http=new Http(cachefd, //0 if no old cache file found
                                      path);
                if(http->tryConnect(sIPv6,host,uri,etag))
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                    #endif
                    Http::pathToHttp[path]=http;
                    http->addClient(this);//into this call, start open cache and stream if partial have started
                }
                else
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
                    #endif
                    status=Status_Idle;
                    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nSocket Error (2)";
                    writeOutput(text,sizeof(text)-1);
                    writeEnd();
                }
            }
        }
    }
}

void Client::startRead()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    if(!readCache->seekToContentPos())
    {
        status=Status_Idle;
        char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nUnable to read cache (1)";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        return;
    }
    readCache->setAsync();
    readCache->seekToContentPos();
    continueRead();
}

void Client::startRead(const std::string &path, const bool &partial)
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    this->partial=partial;
    int cachefd = open(path.c_str(), O_RDWR | O_NOCTTY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    //if failed open cache
    if(cachefd==-1)
    {
        status=Status_Idle;
        char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nUnable to read cache (2)";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        return;
    }
    else
    {
        #ifdef DEBUGFILEOPEN
        std::cerr << "Client::startRead() open: " << path << ", fd: " << cachefd << std::endl;
        #endif
    }
    const off_t &s=lseek(cachefd,1*sizeof(uint64_t),SEEK_SET);
    if(s!=-1)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
        #endif
        const uint64_t &currentTime=time(NULL);
        if(readCache!=nullptr)
        {
            delete readCache;
            readCache=nullptr;
        }
        readCache=new Cache(cachefd);
        readCache->set_access_time(currentTime);
    }
    readCache->seekToContentPos();
    startRead();
}

void Client::tryResumeReadAfterEndOfFile()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    if(partialEndOfFileTrigged)
        continueRead();
}

void Client::continueRead()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    if(readCache==nullptr)
        return;
    if(!dataToWrite.empty())
        return;
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    char buffer[65536-1000];
    do {
        const ssize_t &s=readCache->read(buffer,sizeof(buffer));
        if(s<1)
        {
            if(!partial)
                writeEnd();
            else
            {
                partialEndOfFileTrigged=true;
                //std::cout << "End of file, wait more" << std::endl;
            }
            return;
        }
        partialEndOfFileTrigged=false;
        writeOutput(buffer,s);
        //if can't write all
        if(!dataToWrite.empty())
            return;
        //if can write all, try again
    } while(1);
}

void Client::cacheError()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    status=Status_Idle;
    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nCache file error";
    writeOutput(text,sizeof(text)-1);
    writeEnd();
}

void Client::readyToWrite()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    if(!dataToWrite.empty())
    {
        const ssize_t writedSize=::write(fd,dataToWrite.data(),dataToWrite.size());
        if(errno!=0 && errno!=EAGAIN)
        {
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
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
            std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
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
    write(header,1+1+2+2+2);
    write(data,size);

    if(padding>0)
    {
        char t[padding];
        write(t,padding);
    }
}

void Client::write(const char * const data,const int &size)
{
    if(fd==-1)
        return;
    if(data==nullptr)
        return;
    if(!dataToWrite.empty())
    {
        dataToWrite+=std::string(data,size);
        return;
    }
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
    const int writedSize=::write(fd,data,size);
    if(writedSize==size)
        return;
    else if(errno!=0 && errno!=EAGAIN)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
        #endif
        if(errno!=32)//if not BROKEN PIPE
            std::cerr << fd << ") error to write: " << errno << std::endl;
        disconnect();
        return;
    }
    if(errno==EAGAIN)
    {
        dataToWrite+=std::string(data+writedSize,size-writedSize);
        return;
    }
    else
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
        #endif
        disconnect();
        return;
    }
}

void Client::httpError(const std::string &errorString)
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    const std::string &fullContent=
            "Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nError: "+
            errorString;
    writeOutput(fullContent.data(),fullContent.size());
    writeEnd();
}

void Client::writeEnd()
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << " " << this << std::endl;
    #endif
    if(!outputWrited)
        return;
    if(partial)
        continueRead();
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
