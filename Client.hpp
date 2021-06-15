#ifndef Client_H
#define Client_H

#include "EpollObject.hpp"
#include <string>
#include <netinet/in.h>
#include <unordered_set>

class Cache;
class Http;

class Client : public EpollObject
{
public:
    Client(int cfd);
    ~Client();
    void parseEvent(const epoll_event &event) override;
    void disconnect();
    void disconnectFromHttp();

    void dnsRight(const sockaddr_in6 &sIPv6);
    void dnsError();
    void cacheError();
    void dnsWrong();

    void readyToRead();
    void loadUrl(std::string host,const std::string &uri,const std::string &ifNoneMatch);
    inline bool canAddToPos(const int &i,const int &size,int &pos);
    inline bool read8Bits(uint8_t &var,const char * const data,const int &size,int &pos);
    inline bool read16Bits(uint16_t &var,const char * const data,const int &size,int &pos);
    inline bool read24Bits(uint32_t &var,const char * const data,const int &size,int &pos);

    void readyToWrite();
    void write(const char * const data,const int &size);
    void writeOutput(const char * const data,const int &size);
    void writeEnd();
    void httpError(const std::string &errorString);

    bool startRead();
    bool startRead(const std::string &path, const bool &partial);
    void continueRead();
    void tryResumeReadAfterEndOfFile();
    bool detectTimeout();

    enum Status : uint8_t
    {
        Status_Idle=0x00,
        Status_WaitDns=0x01,
        Status_WaitTheContent=0x02,
    };

    static std::unordered_set<Client *> clients;//for timeout
private:
    int fastcgiid;
    Cache *readCache;
    Http *http;
    std::string dataToWrite;
    bool fullyParsed;
    bool endTriggered;
    Status status;
    bool https;
    bool partial;
    bool partialEndOfFileTrigged;
    bool outputWrited;
    std::string uri;
    std::string host;
    #ifdef MAXFILESIZE
    size_t readSizeFromCache;
    #endif
    uint64_t creationTime;
};

#endif // Client_H
