#ifndef Cache_H
#define Cache_H

#include "EpollObject.hpp"

#include <curl/curl.h>
#include <string>

class Cache;

class Cache : public EpollObject
{
public:
    Cache(const int &fd);
    ~Cache();
    void parseEvent(const epoll_event &event) override;
    uint64_t access_time() const;
    uint64_t last_modification_time_check() const;
    uint16_t http_code() const;
    std::string ETagFrontend() const;//string of 6 char
    std::string ETagBackend() const;
    void set_access_time(const uint64_t &time);
    void set_last_modification_time_check(const uint64_t &time);
    void set_http_code(const uint16_t &http_code);
    void set_ETagFrontend(const std::string &etag);//string of 6 char
    void set_ETagBackend(const std::string &etag);//at end seek to content pos
    void close();
    void setAsync();
    bool seekToContentPos();
    ssize_t write(const char * const data,const size_t &size);
    ssize_t read(char * data,const size_t &size);
    static uint32_t timeToCache(uint16_t http_code);
public://configured by argument, see main.cpp
    /*
     * better performance in flat directory cache, but most FS have problem for more than 30k file on unique folder
     * ext3 and below support up to 32768 files per directory. ext4 supports up to 65536 in the actual count of files
     * Also, the way directories are stored on ext* filesystems is essentially as one big list. On the more modern filesystems (Reiser, XFS, JFS) they are stored as B-trees, which are much more efficient for large sets.
    */
    static bool hostsubfolder;
};

#endif // Cache_H
