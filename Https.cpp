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
    if(backend==nullptr)
        std::cerr << this << ": unable to get backend for " << getUrl() << std::endl;
    #ifdef DEBUGFASTCGI
    std::cerr << this << ": http->backend=" << backend << " && connectInternal=" << connectInternal << std::endl;
    #endif
    return connectInternal && backend!=nullptr;
}

std::unordered_map<std::string,Http *> &Https::pendingList()
{
    return pathToHttps;
}

std::string Https::getUrl()
{
    if(host.empty() && uri.empty())
        return "no url";
    else
        return "https://"+host+uri;
}
