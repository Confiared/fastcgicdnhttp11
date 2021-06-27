#include "CheckTimeout.hpp"
#include "../Http.hpp"
#include "../Https.hpp"
#include "../Backend.hpp"
#include "../Client.hpp"

CheckTimeout::CheckTimeout()
{
}

void CheckTimeout::exec()
{
    for( const auto &n : Backend::addressToHttp )
        for( const auto &m : n.second->busy )
        {
            Backend * p=m;
            #ifdef DEBUGFASTCGI
            /*non sens, can just be disconnected, check data coerancy taking care if connected or not
            if(!p->isValid())
            {
                std::cerr << (void *)p << " !p->isValid() into busy list, error http (abort)" << std::endl;
                abort();
            }*/
            /*can be busy but client/http disconnected
             * if(p->http==nullptr)
            {
                std::cerr << (void *)p << " p->http==null into busy list, error http (abort)" << std::endl;
                abort();
            }*/
            if(p->backendList!=n.second)
            {
                std::cerr << (void *)p << " p->backendList(" << p->backendList << ")!=n.second(" << n.second << "), link backend error (abort)" << std::endl;
                abort();
            }
            #endif
            p->detectTimeout();
        }
    for( const auto &n : Backend::addressToHttps )
        for( const auto &m : n.second->busy )
        {
            Backend * p=m;
            #ifdef DEBUGFASTCGI
            /*non sens, can just be disconnected, check data coerancy taking care if connected or not
            if(!p->isValid())
            {
                std::cerr << (void *)p << " !p->isValid() into busy list, error https (abort)" << std::endl;
                abort();
            }*/
            /*can be busy but client/http disconnected
            if(p->http==nullptr)
            {
                std::cerr << (void *)p << " p->http==null into busy list, error https (abort)" << std::endl;
                abort();
            }*/
            if(p->backendList!=n.second)
            {
                std::cerr << (void *)p << " p->backendList(" << p->backendList << ")!=n.second(" << n.second << "), link backend error (abort)" << std::endl;
                abort();
            }
            #endif
            p->detectTimeout();
        }
    //std::vector<Client *> removeFromClientList;
    for( const auto &n : Client::clients )
    {
        if(n->isValid())
            n->detectTimeout();
        else
        {
            std::cerr << "CheckTimeout::exec() client not valid, disconnect" << std::endl;
            n->disconnect();
            Client::toDelete.insert(n);
            //removeFromClientList.push_back(n);
        }
    }
    /*for( const auto &n : removeFromClientList )
        Client::clients.erase(n); -> generate error of coerancy
            can be not Backend::isValid() after this because Backend::close()  do fd=-1 and Backend::isValid() check this
*/
    {
        std::unordered_map<std::string/* example: cache/29E7336BDEA3327B */,Http *> pathToHttp=Http::pathToHttp;
        for( const auto &n : pathToHttp )
            n.second->detectTimeout();
    }
    {
        std::unordered_map<std::string/* example: cache/29E7336BDEA3327B */,Http *> pathToHttp=Https::pathToHttps;
        for( const auto &n : pathToHttp )
            n.second->detectTimeout();
    }
}
