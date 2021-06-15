#ifndef Dns_H
#define Dns_H

#include "EpollObject.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <map>
#include <sys/socket.h>
#include <netinet/in.h>

class Client;

class Dns : public EpollObject
{
public:
    Dns();
    ~Dns();
    void parseEvent(const epoll_event &event) override;
    inline bool canAddToPos(const int &i, const int &size, int &pos);
    inline bool read8Bits(uint8_t &var, const char * const data, const int &size, int &pos);
    inline bool read16Bits(uint16_t &var, const char * const data, const int &size, int &pos);
    inline bool read16BitsRaw(uint16_t &var, const char * const data, const int &size, int &pos);
    inline bool read32Bits(uint32_t &var, const char * const data, const int &size, int &pos);
    bool tryOpenSocket(std::string line);
    bool get(Client * client,const std::string &host,const bool &https);
    void cancelClient(Client * client,const std::string &host,const bool &https);
    int requestCountMerged();
    void cleanCache();
    void checkQueries();
    static Dns *dns;
    std::string getQueryList() const;
    static const unsigned char include[];
    static const unsigned char exclude[];
private:
    enum StatusEntry : uint8_t
    {
        StatusEntry_Right=0x00,
        StatusEntry_Wrong=0x01,
        StatusEntry_Error=0x02
    };
    struct CacheEntry {
        in6_addr sin6_addr;
        uint64_t outdated_date;/*in s from 1970*/
        StatusEntry status;
    };
    std::unordered_map<std::string,CacheEntry> cache;
    std::map<uint64_t/*outdated_date in s from 1970*/,std::vector<std::string>> cacheByOutdatedDate;
    void addCacheEntryFailed(const StatusEntry &s,const uint32_t &ttl,const std::string &host);
    void addCacheEntry(const StatusEntry &s, const uint32_t &ttl, const std::string &host, const in6_addr &sin6_addr);

    sockaddr_in6 targetDnsIPv6;
    sockaddr_in targetDnsIPv4;
    enum Mode : uint8_t
    {
        Mode_IPv6=0x00,
        Mode_IPv4=0x01,
    };
    Mode mode;
    uint16_t increment;

    struct Query {
        std::string host;
        //separate http and https to improve performance by better caching socket to open
        std::vector<Client *> http;
        std::vector<Client *> https;
        uint8_t retryTime;
        uint64_t nextRetry;
        std::string query;
    };
    int clientInProgress;
    void addQuery(const uint16_t &id,const Query &query);
    void removeQuery(const uint16_t &id, const bool &withNextDueTime=true);
    std::map<uint64_t,std::vector<uint16_t>> queryByNextDueTime;
    std::unordered_map<uint16_t,Query> queryList;
    std::unordered_map<std::string,uint16_t> queryListByHost;
    sockaddr_in6 targetHttp;
    sockaddr_in6 targetHttps;
    in6_addr sin6_addr;
};

#endif // Dns_H
