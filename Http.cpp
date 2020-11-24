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

//ETag -> If-None-Match
const char rChar[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
const size_t &rCharSize=sizeof(rChar)-1;
//Todo: limit max file size 9GB
//reuse cache stale for file <20KB

std::unordered_map<std::string,Http *> Http::pathToHttp;
int Http::fdRandom=-1;
char Http::buffer[4096];

Http::Http(const int &cachefd, //0 if no old cache file found
           const std::string &cachePath) :
    cachePath(cachePath),//to remove from Http::pathToHttp
    tempCache(nullptr),
    finalCache(nullptr),
    parsedHeader(false),
    contentsize(-1),
    contentwritten(0),
    http_code(0),
    parsing(Parsing_None),
    requestSended(false),
    backend(nullptr),
    contentLengthPos(-1),
    chunkLength(-1)
{
    #ifdef DEBUGFASTCGI
    std::cerr << "contructor " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
    if(cachePath.empty())
        abort();
    #endif
    if(cachefd==0)
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
        client->writeEnd();
    clientsList.clear();
}

bool Http::tryConnect(const sockaddr_in6 &s,const std::string &host,const std::string &uri,const std::string &etag)
{
    #ifdef DEBUGFASTCGI
    std::cerr << "try connect " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
    if(etag.find('\0')!=std::string::npos)
    {
        std::cerr << "etag contain \\0 abort" << __FILE__ << ":" << __LINE__ << std::endl;
        abort();
    }
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
    #ifdef DEBUGFASTCGI
    std::cerr << this << ": http->backend=" << backend << std::endl;
    #endif
    return connectInternal;
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
    std::cout << "Http::sendRequest(): " << this << std::endl;
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
    std::cout << "uri: " << uri << std::endl;
    if(uri.empty())
    {
        std::cerr << "Http::readyToWrite(): but uri.empty()" << std::endl;
        abort();
    }
    #endif
    requestSended=true;
    if(etagBackend.empty())
    {
        std::string h(std::string("GET ")+uri+" HTTP/1.1\nHOST: "+host+"\n"
              #ifdef HTTPGZIP
              "Accept-Encoding: gzip\n"+
              #endif
                      "\n");
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
        std::cerr << h << std::endl;
        #endif
        if(!socketWrite(h.data(),h.size()))
            std::cerr << "ERROR to write: " << h << std::endl;
    }
    else
    {
        std::string h(std::string("GET ")+uri+" HTTP/1.1\nHOST: "+host+"\nIf-None-Match: "+etagBackend+"\n"
              #ifdef HTTPGZIP
              "Accept-Encoding: gzip\n"+
              #endif
                      "\n");
        #ifdef DEBUGFASTCGI
        std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
        std::cerr << h << std::endl;
        #endif
        if(!socketWrite(h.data(),h.size()))
            std::cerr << "ERROR to write: " << h << std::endl;
    }
    /*used for retry host.clear();
    uri.clear();*/
}

char Http::randomETagChar(uint8_t r)
{
    #ifdef DEBUGFASTCGI
    if(rCharSize!=65)
    {
        std::cerr << __FILE__ << ":" << __LINE__ << " wrong rChar size abort" << std::endl;
        abort();
    }
    #endif
    return rChar[r%rCharSize];
}

void Http::readyToRead()
{
/*    if(var=="content-length")
    if(var=="content-type")*/
    //::read(Http::buffer

    //load into buffer the previous content
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
        const ssize_t size=socketRead(buffer+offset,sizeof(buffer)-offset);
        readSize=size;
        #ifdef DEBUGFASTCGI
        std::cout << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
        if(size>0)
        {
            //std::cout << std::string(buffer,size) << std::endl;
            if(parsing==Parsing_Content)
            {
                bool endDetected=false;
                write(buffer,size,endDetected);
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
                                    #ifdef DEBUGFILEOPEN
                                    std::cerr << "Http::readyToRead() open: " << cachePath << ", fd: " << cachefd << " " << __FILE__ << ":" << __LINE__ << std::endl;
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
                                        {
                                            std::cerr << "etag will contain \\0 abort" << __FILE__ << ":" << __LINE__ << std::endl;
                                            abort();
                                        }
                                        r+=c;
                                        rIndex++;
                                    }
                                }

                                const int64_t &currentTime=time(NULL);
                                tempCache->set_access_time(currentTime);
                                tempCache->set_last_modification_time_check(currentTime);
                                tempCache->set_http_code(http_code);
                                tempCache->set_ETagFrontend(r);//string of 6 char
                                tempCache->set_ETagBackend(etagBackend);//at end seek to content pos

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
                                        header+=
                                        "Date: "+timestampsToHttpDate(currentTime)+"\n"
                                        "Expires: "+timestampsToHttpDate(currentTime+Cache::timeToCache(http_code))+"\n"
                                        "Cache-Control: public\n"
                                        "ETag: \""+r+"\"\n"
                                        "Access-Control-Allow-Origin: *\n";
                                }
                                #ifdef DEBUGFASTCGI
                                std::cout << "header: " << header << std::endl;
                                #endif
                                header+="\n";
                                tempCache->seekToContentPos();
                                if(tempCache->write(header.data(),header.size())!=(ssize_t)header.size())
                                    abort();

                                epoll_event event;
                                memset(&event,0,sizeof(event));
                                event.data.ptr = tempCache;
                                event.events = EPOLLOUT;
                                //std::cerr << "EPOLL_CTL_ADD bis: " << cachefd << std::endl;

                                //tempCache->setAsync(); -> to hard for now

                                for(Client * client : clientsList)
                                    client->startRead(cachePath+".tmp",true);

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
                    //std::cerr << "content: " << std::string(buffer+pos,size-pos) << std::endl;
                    bool endDetected=false;
                    write(buffer+pos,size-pos,endDetected);
                    if(endDetected)
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
                                    abort();
                                }
                            }
                        break;
                        default:
                        std::cerr << "parsing var before abort over size: " << (int)parsing << std::endl;
                        abort();
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
            std::cout << "socketRead(), errno " << errno << std::endl;
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
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
    std::cout << "uri: " << uri << std::endl;
    if(uri.empty())
    {
        std::cerr << "Http::readyToWrite(): but uri.empty()" << std::endl;
        std::cerr << "readyToWrite " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
        abort();
    }
    #endif
    if(!requestSended)
        sendRequest();
    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
    std::cout << "uri: " << uri << std::endl;
    #endif
}

ssize_t Http::socketRead(void *buffer, size_t size)
{
    if(backend==nullptr)
        return -1;
    return backend->socketRead(buffer,size);
}

bool Http::socketWrite(const void *buffer, size_t size)
{
    if(backend==nullptr)
    {
        std::cerr << "Http::socketWrite error backend==nullptr" << std::endl;
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
    for(Client * client : clientsList)
        client->writeEnd();
    clientsList.clear();
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
        if(pathToHttp.find(cachePath)!=pathToHttp.cend())
            pathToHttp.erase(cachePath);

    contenttype.clear();
    url.clear();
    headerBuff.clear();
}

bool Http::haveUrlAndFrontendConnected()
{
    return !host.empty() && !uri.empty() && !clientsList.empty();
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
        if(finalCache!=nullptr)
            finalCache->set_last_modification_time_check(time(NULL));
        //send file to listener
        for(Client * client : clientsList)
        {
            #ifdef DEBUGFASTCGI
            std::cout << "send file to listener: " << client << std::endl;
            #endif
            client->startRead(cachePath,false);
        }
        return false;
    }
    const std::string &errorString("Http "+std::to_string(errorCode));
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
    std::unordered_map<std::string,Http *> &pathToHttp=pendingList();
    pathToHttp.erase(url);
    return false;
    //disconnectSocket();
}

void Http::flushRead()
{
    disconnectBackend();
    disconnectFrontend();
    while(socketRead(Http::buffer,sizeof(Http::buffer))==sizeof(Http::buffer))
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
    if(finalCache!=nullptr)
        finalCache->close();
    const char * const cstr=cachePath.c_str();
    //todo, optimise with renameat2(RENAME_EXCHANGE) if --flatcache + destination
    #ifdef DEBUGFILEOPEN
    std::cerr << "Http::disconnectBackend() post, tempCache close: " << tempCache << std::endl;
    #endif
    if(tempCache!=nullptr)
    {
        tempCache->close();
        ::unlink(cstr);
        if(rename((cachePath+".tmp").c_str(),cstr)==-1)
            std::cerr << "unable to move " << cachePath << ".tmp to " << cachePath << ", errno: " << errno << std::endl;
        //disable to cache
        //::unlink(cstr);
    }

    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    backend->downloadFinished();
    #ifdef DEBUGFASTCGI
    if(backend!=nullptr && backend->http==this)
    {
        std::cerr << this << ": backend->http==this, backend: " << backend << std::endl;
        abort();
    }
    std::cerr << this << ": backend=nullptr" << std::endl;
    #endif
    backend=nullptr;

    host.clear();
    uri.clear();
    #ifdef DEBUGFASTCGI
    std::cerr << "disconnectBackend " << this << " uri: " << uri << ": " << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
}

void Http::addClient(Client * client)
{
    #ifdef DEBUGFASTCGI
    std::cerr << this << " " << __FILE__ << ":" << __LINE__ << " add client: " << client << std::endl;
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

int Http::write(const char * const data,const size_t &size,bool &endDetected)
{
    endDetected=false;
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
                        chunkHeader+=std::string(data+pos2,size-pos2);
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
    char buffer[100];
    struct tm *my_tm = gmtime(&time);
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", my_tm);
    return buffer;
}
