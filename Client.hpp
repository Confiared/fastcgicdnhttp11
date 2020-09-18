#ifndef Client_H
#define Client_H

#include "EpollObject.hpp"
#include <string>
#include <netinet/in.h>

class Cache;
class Http;

class Client : public EpollObject
{
public:
    Client(int cfd);
    ~Client();
    void parseEvent(const epoll_event &event) override;
    void disconnect();

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

    void startRead();
    void startRead(const std::string &path, const bool &partial);
    void continueRead();
    void tryResumeReadAfterEndOfFile();

    enum Status : uint8_t
    {
        Status_Idle=0x00,
        Status_WaitDns=0x01,
        Status_WaitTheContent=0x02,
    };
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
};

#endif // Client_H
