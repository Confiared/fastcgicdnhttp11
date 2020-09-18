#include "Https.hpp"
#include <iostream>

std::unordered_map<std::string,Http *> Https::pathToHttps;

Https::Https(const int &cachefd, const std::string &cachePath) :
    Http(cachefd,cachePath)
{
}

bool Https::tryConnectInternal(const sockaddr_in6 &s)
{
    bool connectInternal=false;
    backend=Backend::tryConnectHttps(s,this,connectInternal);
    std::cerr << this << ": http->backend=" << backend << std::endl;
    return connectInternal;
}

std::unordered_map<std::string,Http *> &Https::pendingList()
{
    return pathToHttps;
}
