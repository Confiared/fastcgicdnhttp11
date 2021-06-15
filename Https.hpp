#ifndef HTTPS_H
#define HTTPS_H

#include "Http.hpp"
#include <openssl/ssl.h>

class Https : public Http
{
public:
    Https(const int &cachefd,//0 if no old cache file found
          const std::string &cachePath);
    bool tryConnectInternal(const sockaddr_in6 &s);
    std::unordered_map<std::string,Http *> &pendingList();
    #ifdef DEBUGHTTPS
    static void dump_cert_info(SSL *ssl, bool server);
    #endif
    void init_ssl_opts(SSL_CTX* ctx);
    std::string getUrl() const;
public:
    static std::unordered_map<std::string,Http *> pathToHttps;
};

#endif // HTTPS_H
