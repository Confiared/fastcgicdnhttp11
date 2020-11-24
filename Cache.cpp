#include "Cache.hpp"
#include <unistd.h>
#include <iostream>
#include <fcntl.h>

bool Cache::hostsubfolder=true;
/*uint32_t Cache::maxiumSizeToBeSmallFile=4096;
uint64_t Cache::maxiumSmallFileCacheSize=0;//diable by default (to be safe if on ram disk)
uint64_t Cache::smallFileCacheSize=0;*/

//use pread, pwrite

/*Format, insert/drop at middle with sequential scan:
 * 64Bits: access time
 * 64Bits: last modification time check (Modification based on ETag)
 * 16Bits: http code
 * 48Bits: frontend content Etag, Base64 Random bytes at Creation or Modification
 * 8Bits: Etag backend size in Bytes
 * XBytes: backend content (see Etag backend size)
 * Http Headers ended with \n\n
 * Http body
*/

Cache::Cache(const int &fd)
{
    this->kind=EpollObject::Kind::Kind_Cache;
    this->fd=fd;
    /*
    //while receive write to cache
    //when finish
        //unset curl to all future listener
        //Close all listener
    */
}

Cache::~Cache()
{
    close();
}

void Cache::parseEvent(const epoll_event &event)
{
    (void)event;
/*    if(!(event & EPOLLIN))
        readyToRead();*/
}

void Cache::close()
{
    #ifdef DEBUGFILEOPEN
    std::cerr << "Cache::close(), fd: " << fd << std::endl;
    #endif
    if(fd!=-1)
    {
        epoll_ctl(epollfd,EPOLL_CTL_DEL, fd, NULL);
        ::close(fd);
        fd=-1;
    }
}

uint64_t Cache::access_time() const
{
    uint64_t time=0;
    if(::pread(fd,&time,sizeof(time),0)==sizeof(time))
        return time;
    return 0;
}

uint64_t Cache::last_modification_time_check() const
{
    uint64_t time=0;
    if(::pread(fd,&time,sizeof(time),sizeof(uint64_t))==sizeof(time))
        return time;
    return 0;
}

std::string Cache::ETagFrontend() const
{
    char randomIndex[6];
    if(::pread(fd,randomIndex,sizeof(randomIndex),2*sizeof(uint64_t)+sizeof(uint16_t))==sizeof(randomIndex))
    {
        const std::string &etag=std::string(randomIndex,sizeof(randomIndex));
        #ifdef DEBUGFASTCGI
        if(etag.find('\0')!=std::string::npos)
        {
            std::cerr << "etag contain \\0 abort" << __FILE__ << ":" << __LINE__ << std::endl;
            abort();
        }
        #endif
        return etag;
    }
    return std::string();
}

std::string Cache::ETagBackend() const
{
    uint8_t etagBackendSize=0;
    if(::pread(fd,&etagBackendSize,sizeof(etagBackendSize),3*sizeof(uint64_t))==sizeof(etagBackendSize))
    {
        char buffer[etagBackendSize];
        if(::pread(fd,buffer,etagBackendSize,3*sizeof(uint64_t)+sizeof(uint8_t))==etagBackendSize)
        {
            const std::string &etag=std::string(buffer,etagBackendSize);
            #ifdef DEBUGFASTCGI
            if(etag.find('\0')!=std::string::npos)
            {
                std::cerr << "etag contain \\0 abort" << __FILE__ << ":" << __LINE__ << std::endl;
                abort();
            }
            #endif
            return etag;
        }
    }
    return std::string();
}

uint16_t Cache::http_code() const
{
    uint64_t time=0;
    if(::pread(fd,&time,sizeof(time),2*sizeof(uint64_t))==sizeof(time))
        return time;
    return 500;
}

void Cache::set_access_time(const uint64_t &time)
{
    if(::pwrite(fd,&time,sizeof(time),0)!=sizeof(time))
    {
        std::cerr << "Unable to write last_modification_time_check" << std::endl;
        return;
    }
}

void Cache::set_last_modification_time_check(const uint64_t &time)
{
    if(::pwrite(fd,&time,sizeof(time),sizeof(uint64_t))!=sizeof(time))
    {
        std::cerr << "Unable to write last_modification_time_check" << std::endl;
        return;
    }
}

void Cache::set_ETagFrontend(const std::string &etag)
{
    #ifdef DEBUGFASTCGI
    if(etag.find('\0')!=std::string::npos)
    {
        std::cerr << "etag contain \\0 abort" << __FILE__ << ":" << __LINE__ << std::endl;
        abort();
    }
    #endif
    if((size_t)::pwrite(fd,etag.data(),etag.size(),2*sizeof(uint64_t)+sizeof(uint16_t))!=etag.size())
    {
        std::cerr << "Unable to write last_modification_time_check" << std::endl;
        return;
    }
}

void Cache::set_ETagBackend(const std::string &etag)//at end seek to content pos
{
    #ifdef DEBUGFASTCGI
    if(etag.find('\0')!=std::string::npos)
    {
        std::cerr << "etag contain \\0 abort" << __FILE__ << ":" << __LINE__ << std::endl;
        abort();
    }
    #endif
    if(etag.size()>255)
    {
        char c=0x00;
        if(::pwrite(fd,&c,sizeof(c),3*sizeof(uint64_t))!=sizeof(c))
        {
            std::cerr << "Unable to write last_modification_time_check" << std::endl;
            return;
        }
    }
    else
    {
        char c=etag.size();
        if(::pwrite(fd,&c,sizeof(c),3*sizeof(uint64_t))!=sizeof(c))
        {
            std::cerr << "Unable to write last_modification_time_check" << std::endl;
            return;
        }
        if(c>0)
        {
            if((size_t)::pwrite(fd,etag.data(),etag.size(),3*sizeof(uint64_t)+sizeof(uint8_t))!=etag.size())
            {
                std::cerr << "Unable to write last_modification_time_check" << std::endl;
                return;
            }
        }
    }
}

void Cache::set_http_code(const uint16_t &http_code)
{
    if(::pwrite(fd,&http_code,sizeof(http_code),2*sizeof(uint64_t))!=sizeof(http_code))
    {
        std::cerr << "Unable to write last_modification_time_check" << std::endl;
        return;
    }
}

void Cache::setAsync()
{
    int flags, s;
    flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1)
        std::cerr << "fcntl get flags error" << std::endl;
    else
    {
        flags |= O_NONBLOCK;
        s = fcntl(fd, F_SETFL, flags);
        if(s == -1)
            std::cerr << "fcntl set flags error" << std::endl;
    }
}

bool Cache::seekToContentPos()
{
    uint8_t etagBackendSize=0;
    if(::pread(fd,&etagBackendSize,sizeof(etagBackendSize),3*sizeof(uint64_t))==sizeof(etagBackendSize))
    {
        const uint16_t &pos=3*sizeof(uint64_t)+sizeof(uint8_t)+etagBackendSize;
        const off_t &s=lseek(fd,pos,SEEK_SET);
        if(s==-1)
        {
            std::cerr << "Unable to seek setContentPos" << std::endl;
            return false;
        }
        //std::cout << "seek to:" << pos << std::endl;
        return true;
    }
    return false;
}

ssize_t Cache::write(const char * const data,const size_t &size)
{
    return ::write(fd,data,size);
}

ssize_t Cache::read(char * data,const size_t &size)
{
    return ::read(fd,data,size);
}

uint32_t Cache::timeToCache(uint16_t http_code)
{
    switch(http_code)
    {
        case 200:
            return 24*3600;
        break;
        default:
            return 60;
        break;
    }
}
