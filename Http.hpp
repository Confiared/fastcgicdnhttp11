#ifndef HTTP_H
#define HTTP_H

#include <string>
#include <vector>
#include <netinet/in.h>
#include <unordered_map>
#ifdef MAXFILESIZE
#include <unordered_set>
#endif

#include "EpollObject.hpp"
#include "Backend.hpp"

class Client;
class Cache;

class Http
{
public:
    Http(const int &cachefd,//0 if no old cache file found
         const std::string &cachePath);
    virtual ~Http();
    bool tryConnect(const sockaddr_in6 &s, const std::string &host, const std::string &uri, const std::string &etagBackend=std::string());
    virtual bool tryConnectInternal(const sockaddr_in6 &s);
    void parseEvent(const epoll_event &event);
    static char randomETagChar(uint8_t r);
    void sendRequest();
    void readyToRead();
    void readyToWrite();
    void flushRead();
    void disconnectFrontend();
    virtual std::unordered_map<std::string,Http *> &pendingList();
    void disconnectBackend();
    const int &getAction() const;
    int write(const char * const data, const size_t &size);
    static std::string timestampsToHttpDate(const int64_t &time);
    void addClient(Client * client);
    bool removeClient(Client * client);
    const std::string &getCachePath() const;
    void resetRequestSended();
    bool haveUrlAndFrontendConnected() const;
    bool isAlive() const;
    bool HttpReturnCode(const int &errorCode);
    bool backendError(const std::string &errorString);
    virtual std::string getUrl() const;
    void parseNonHttpError(const Backend::NonHttpError &error);
    bool detectTimeout();
    std::string getQuery() const;

    ssize_t socketRead(void *buffer, size_t size);
    bool socketWrite(const void *buffer, size_t size);
public:
    static std::unordered_map<std::string,Http *> pathToHttp;
    static int fdRandom;
    #ifdef MAXFILESIZE
    static std::unordered_map<std::string,Http *> duplicateOpen;
    std::string cachePath;
    #endif
    static char buffer[1024*1024];
private:
    std::vector<Client *> clientsList;
    #ifndef MAXFILESIZE
    std::string cachePath;
    #endif
    Cache *tempCache;
    Cache *finalCache;
    bool parsedHeader;
    uint64_t lastReceivedBytesTimestamps;

    std::string contenttype;
    std::string url;
    int64_t contentsize;
    int64_t contentwritten;
    std::string headerBuff;
    uint16_t http_code;
    enum Parsing: uint8_t
    {
        Parsing_None,
        Parsing_HeaderVar,
        Parsing_HeaderVal,
        Parsing_RemoteAddr,
        Parsing_ServerAddr,
        Parsing_ContentLength,
        Parsing_ContentType,
        #ifdef HTTPGZIP
        Parsing_ContentEncoding,
        #endif
        Parsing_ETag,
        Parsing_Content
    };
    Parsing parsing;

    std::string etagBackend;
    std::string remoteAddr;
protected:
    std::string host;
    std::string uri;
public:
    bool requestSended;
    bool headerWriten;
    bool endDetected;
    Backend *backend;
    int64_t contentLengthPos;
    int64_t chunkLength;
    std::string chunkHeader;
    #ifdef HTTPGZIP
    std::string contentEncoding;
    #endif
private:
    sockaddr_in6 m_socket;
};

#endif // HTTP_H
