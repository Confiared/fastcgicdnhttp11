#include "CheckTimeout.hpp"
#include "../Http.hpp"
#include "../Https.hpp"
#include "../Backend.hpp"

CheckTimeout::CheckTimeout()
{
}

void CheckTimeout::exec()
{
    for( const auto &n : Http::pathToHttp )
        n.second->detectTimeout();
    for( const auto &n : Https::pathToHttps )
        n.second->detectTimeout();
    for( const auto &n : Backend::addressToHttp )
        for( const auto &m : n.second->busy )
            m->detectTimeout();
    for( const auto &n : Backend::addressToHttps )
        for( const auto &m : n.second->busy )
            m->detectTimeout();
}
