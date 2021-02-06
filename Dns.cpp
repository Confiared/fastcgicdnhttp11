#include "Dns.hpp"
#include "Client.hpp"
#include <unistd.h>
#include <iostream>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <fcntl.h>

struct __attribute__ ((__packed__)) dns_query {
    uint16_t id;
    uint16_t flags;
    uint16_t question_count;
    uint16_t answer_count;
    uint16_t authority_count;
    uint16_t add_count;
    uint8_t  payload[];
};

Dns *Dns::dns=nullptr;
const unsigned char Dns::include[]={0x28,0x03,0x19,0x20};
const unsigned char Dns::exclude[]={0x28,0x03,0x19,0x20,0x00,0x00,0x00,0x00,0xb4,0xb2,0x5f,0x61,0xd3,0x7f};

Dns::Dns()
{
    memset(&targetHttp,0,sizeof(targetHttp));
    targetHttp.sin6_port = htobe16(80);
    targetHttp.sin6_family = AF_INET6;

    memset(&targetHttps,0,sizeof(targetHttps));
    targetHttps.sin6_port = htobe16(443);
    targetHttps.sin6_family = AF_INET6;

    clientInProgress=0;
    this->kind=EpollObject::Kind::Kind_Dns;

    memset(&targetDnsIPv6, 0, sizeof(targetDnsIPv6));
    targetDnsIPv6.sin6_port = htobe16(53);
    memset(&targetDnsIPv4, 0, sizeof(targetDnsIPv4));
    targetDnsIPv4.sin_port = htobe16(53);

    memset(&sin6_addr,0,sizeof(sin6_addr));

    //read resolv.conf
    {
        FILE * fp;
        char * line = NULL;
        size_t len = 0;
        ssize_t read;

        fp = fopen("/etc/resolv.conf", "r");
        if (fp == NULL)
        {
            std::cerr << "Unable to open /etc/resolv.conf" << std::endl;
            exit(EXIT_FAILURE);
        }

        while ((read = getline(&line, &len, fp)) != -1) {
            //create udp socket to dns server
            if(tryOpenSocket(line))
                break;
        }

        fclose(fp);
        if (line)
            free(line);
    }

    //add to event loop
    epoll_event event;
    event.data.ptr = this;
    event.events = EPOLLIN;
    //std::cerr << "EPOLL_CTL_ADD: " << fd << std::endl;
    if(epoll_ctl(epollfd,EPOLL_CTL_ADD, fd, &event) == -1)
    {
        printf("epoll_ctl failed to add server: %s", strerror(errno));
        abort();
    }

    increment=1;
}

Dns::~Dns()
{
}

bool Dns::tryOpenSocket(std::string line)
{
    std::string prefix=line.substr(0,11);
    if(prefix=="nameserver ")
    {
        line=line.substr(11);
        line.resize(line.size()-1);
        const std::string &host=line;

        memset(&targetDnsIPv6, 0, sizeof(targetDnsIPv6));
        targetDnsIPv6.sin6_port = htobe16(53);
        const char * const hostC=host.c_str();
        int convertResult=inet_pton(AF_INET6,hostC,&targetDnsIPv6.sin6_addr);
        if(convertResult!=1)
        {
            memset(&targetDnsIPv4, 0, sizeof(targetDnsIPv4));
            targetDnsIPv4.sin_port = htobe16(53);
            convertResult=inet_pton(AF_INET,hostC,&targetDnsIPv4.sin_addr);
            if(convertResult!=1)
            {
                std::cerr << "not IPv4 and IPv6 address, host: \"" << host << "\", portstring: \"53\", errno: " << std::to_string(errno) << std::endl;
                abort();
            }
            else
            {
                targetDnsIPv4.sin_family = AF_INET;

                fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (fd == -1)
                {
                    std::cerr << "unable to create UDP socket" << std::endl;
                    abort();
                }
                sockaddr_in si_me;
                memset((char *) &si_me, 0, sizeof(si_me));
                si_me.sin_family = AF_INET;
                si_me.sin_port = htons(50053);
                si_me.sin_addr.s_addr = htonl(INADDR_ANY);
                if(bind(fd,(struct sockaddr*)&si_me, sizeof(si_me))==-1)
                {
                    std::cerr << "unable to bind UDP socket" << std::endl;
                    abort();
                }

                int flags, s;
                flags = fcntl(fd, F_GETFL, 0);
                if(flags == -1)
                    std::cerr << "fcntl get flags error" << std::endl;
                else
                {
                    flags |= O_NONBLOCK;
                    s = fcntl(fd, F_SETFL, flags);
                    if(s == -1)
                        std::cerr << "fcntl set flags error" << std::endl;
                }

                mode=Mode_IPv4;
                return true;
            }
        }
        else
        {
            targetDnsIPv6.sin6_family = AF_INET6;

            fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
            if (fd == -1)
            {
                std::cerr << "unable to create UDP socket" << std::endl;
                abort();
            }
            sockaddr_in6 si_me;
            memset((char *) &si_me, 0, sizeof(si_me));
            si_me.sin6_family = AF_INET6;
            si_me.sin6_port = htons(50053);
            si_me.sin6_addr = IN6ADDR_ANY_INIT;
            if(bind(fd,(struct sockaddr*)&si_me, sizeof(si_me))==-1)
            {
                std::cerr << "unable to bind UDP socket" << std::endl;
                abort();
            }

            int flags, s;
            flags = fcntl(fd, F_GETFL, 0);
            if(flags == -1)
                std::cerr << "fcntl get flags error" << std::endl;
            else
            {
                flags |= O_NONBLOCK;
                s = fcntl(fd, F_SETFL, flags);
                if(s == -1)
                    std::cerr << "fcntl set flags error" << std::endl;
            }

            mode=Mode_IPv6;
            return true;
        }
    }
    return false;
}

void Dns::parseEvent(const epoll_event &event)
{
    if(event.events & EPOLLIN)
    {
        int size = 0;
        do
        {
            char buffer[4096];
            if(mode==Mode_IPv6)
            {
                sockaddr_in6 si_other;
                unsigned int slen = sizeof(si_other);
                memset(&si_other,0,sizeof(si_other));
                size = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr *) &si_other, &slen);
                if(size<0)
                    break;
                if(memcmp(&targetDnsIPv6.sin6_addr,&si_other.sin6_addr,16)!=0)
                    return;
            }
            else //if(mode==Mode_IPv4)
            {
                sockaddr_in si_other;
                unsigned int slen = sizeof(si_other);
                memset(&si_other,0,sizeof(si_other));
                size = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr *) &si_other, &slen);
                if(memcmp(&targetDnsIPv4.sin_addr,&si_other.sin_addr,4)!=0)
                    return;
            }
            clientInProgress--;

            int pos=0;
            uint16_t transactionId=0;
            if(!read16BitsRaw(transactionId,buffer,size,pos))
                return;
            uint16_t flags=0;
            if(!read16Bits(flags,buffer,size,pos))
                return;
            uint16_t questions=0;
            if(!read16Bits(questions,buffer,size,pos))
                return;
            uint16_t answersIndex=0;
            uint16_t answers=0;
            if(!read16Bits(answers,buffer,size,pos))
                return;
            if(!canAddToPos(2+2,size,pos))
                return;

            //skip query
            uint8_t len,offs=0;
            while((offs<(size-pos)) && (len = buffer[pos+offs]))
                offs += len+1;
            pos+=offs+1;
            uint16_t type=0;
            if(!read16Bits(type,buffer,size,pos))
                return;
            if(type!=0x001c)
                return;
            uint16_t classIn=0;
            if(!read16Bits(classIn,buffer,size,pos))
                return;
            if(classIn!=0x0001)
                return;


            //answers list
            if(queryList.find(transactionId)!=queryList.cend())
            {
                const Query &q=queryList.at(transactionId);
                const std::vector<Client *> &http=q.http;
                const std::vector<Client *> &https=q.https;
                //std::string hostcpp(std::string hostcpp(q.host));-> not needed
                if(!http.empty() || !https.empty())
                {
                    bool clientsFlushed=false;

                    if((flags & 0x000F)==0x0001)
                    {
                        if(!clientsFlushed)
                        {
                            clientsFlushed=true;
                            //addCacheEntry(StatusEntry_Wrong,0,q.host);-> wrong string to resolve, host is not dns valid
                            for(Client * const c : http)
                                c->dnsError();
                            for(Client * const c : https)
                                c->dnsError();
                            removeQuery(transactionId);
                        }
                    }
                    else if((flags & 0xFA0F)!=0x8000)
                    {
                        if(!clientsFlushed)
                        {
                            clientsFlushed=true;
                            addCacheEntryFailed(StatusEntry_Wrong,0,q.host);
                            for(Client * const c : http)
                                c->dnsError();
                            for(Client * const c : https)
                                c->dnsError();
                            removeQuery(transactionId);
                        }
                    }
                    else
                    {
                        while(answersIndex<answers)
                        {
                            uint16_t AName=0;
                            if(!read16Bits(AName,buffer,size,pos))
                                return;
                            uint16_t type=0;
                            if(!read16Bits(type,buffer,size,pos))
                                if(!clientsFlushed)
                                {
                                    clientsFlushed=true;
                                    addCacheEntryFailed(StatusEntry_Error,3600,q.host);
                                    for(Client * const c : http)
                                        c->dnsError();
                                    for(Client * const c : https)
                                        c->dnsError();
                                    removeQuery(transactionId);
                                }
                            switch(type)
                            {
                                //AAAA
                                case 0x001c:
                                {
                                    uint16_t classIn=0;
                                    if(!read16Bits(classIn,buffer,size,pos))
                                        return;
                                    if(classIn!=0x0001)
                                        break;
                                    uint32_t ttl=0;
                                    if(!read32Bits(ttl,buffer,size,pos))
                                        return;
                                    uint16_t datasize=0;
                                    if(!read16Bits(datasize,buffer,size,pos))
                                        return;
                                    if(datasize!=16)
                                        return;

                                    //TODO saveToCache();
                                    if(memcmp(buffer+pos,Dns::include,sizeof(Dns::include))!=0 || memcmp(buffer+pos,Dns::exclude,sizeof(Dns::exclude))==0)
                                    {
                                        if(!clientsFlushed)
                                        {
                                            clientsFlushed=true;
                                            addCacheEntry(StatusEntry_Wrong,ttl,q.host,*reinterpret_cast<in6_addr *>(buffer+pos));
                                            for(Client * const c : http)
                                                c->dnsWrong();
                                            for(Client * const c : https)
                                                c->dnsWrong();
                                            removeQuery(transactionId);
                                        }
                                    }
                                    else
                                    {
                                        if(!clientsFlushed)
                                        {
                                            clientsFlushed=true;
                                            addCacheEntry(StatusEntry_Right,ttl,q.host,*reinterpret_cast<in6_addr *>(buffer+pos));

                                            if(!http.empty())
                                            {
                                                memcpy(&targetHttp.sin6_addr,buffer+pos,16);
                                                for(Client * const c : http)
                                                    c->dnsRight(targetHttp);
                                            }
                                            if(!https.empty())
                                            {
                                                memcpy(&targetHttps.sin6_addr,buffer+pos,16);
                                                for(Client * const c : https)
                                                    c->dnsRight(targetHttps);
                                            }

                                            removeQuery(transactionId);
                                        }
                                    }
                                }
                                break;
                                default:
                                {
                                    canAddToPos(2+4,size,pos);
                                    uint16_t datasize=0;
                                    if(!read16Bits(datasize,buffer,size,pos))
                                        return;
                                    canAddToPos(datasize,size,pos);
                                }
                                break;
                            }
                            answersIndex++;
                        }
                        if(!clientsFlushed)
                        {
                            clientsFlushed=true;
                            addCacheEntryFailed(StatusEntry_Error,3600,q.host);
                            for(Client * const c : http)
                                c->dnsError();
                            for(Client * const c : https)
                                c->dnsError();
                            removeQuery(transactionId);
                        }
                    }
                }
            }
        } while(size>=0);
    }
}

void Dns::cleanCache()
{
    const std::map<uint64_t/*outdated_date in s from 1970*/,std::vector<std::string>> cacheByOutdatedDate=this->cacheByOutdatedDate;
    for (auto const& x : cacheByOutdatedDate)
    {
        const uint64_t t=x.first;
        if(t>(uint64_t)time(NULL))
            return;
        const std::vector<std::string> &list=x.second;
        for (auto const& host : list)
            cache.erase(host);
        this->cacheByOutdatedDate.erase(t);
    }
}

void Dns::addCacheEntryFailed(const StatusEntry &s,const uint32_t &ttl,const std::string &host)
{
    #ifdef DEBUGFASTCGI
    if(s==StatusEntry_Right)
    {
        std::cerr << "Can't call right without IP" << std::endl;
        abort();
    }
    #endif
    addCacheEntry(s,ttl,host,sin6_addr);
}

void Dns::addCacheEntry(const StatusEntry &s,const uint32_t &ttl,const std::string &host,const in6_addr &sin6_addr)
{
    //prevent DDOS due to out of memory situation
    if(cache.size()>5000)
        return;

    //remove old entry from cacheByOutdatedDate
    if(cache.find(host)!=cache.cend())
    {
        const CacheEntry &e=cache.at(host);
        std::vector<std::string> &list=cacheByOutdatedDate[e.outdated_date];
        for (size_t i = 0; i < list.size(); i++) {
            const std::string &s=list.at(i);
            if(s==host)
            {
                list.erase(list.cbegin()+i);
                break;
            }
        }
    }

    CacheEntry &entry=cache[host];
    if(ttl<24*3600)
    {
        //in case of wrong ttl, ttl too short or dns error
        if(ttl<5*60)
        {
            if(s==StatusEntry_Right)
                entry.outdated_date=time(NULL)+5*60;
            else
                entry.outdated_date=time(NULL)+3600;
        }
    }
    else
        entry.outdated_date=time(NULL)+24*3600;
    entry.status=s;

    #ifdef DEBUGFASTCGI
    if(s==StatusEntry_Right)
    {
        char astring[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &(sin6_addr), astring, INET6_ADDRSTRLEN);
        if(std::string(astring)=="::")
        {
            std::cerr << "Internal error, try connect on ::" << std::endl;
            abort();
        }
    }
    #endif

    memcpy(&entry.sin6_addr,&sin6_addr,sizeof(in6_addr));

    //insert entry to cacheByOutdatedDate
    cacheByOutdatedDate[entry.outdated_date].push_back(host);
}

bool Dns::canAddToPos(const int &i, const int &size, int &pos)
{
    if((pos+i)>size)
        return false;
    pos+=i;
    return true;
}

bool Dns::read8Bits(uint8_t &var, const char * const data, const int &size, int &pos)
{
    if((pos+(int)sizeof(var))>size)
        return false;
    var=data[pos];
    pos+=sizeof(var);
    return true;
}

bool Dns::read16Bits(uint16_t &var, const char * const data, const int &size, int &pos)
{
    uint16_t t=0;
    read16BitsRaw(t,data,size,pos);
    var=be16toh(t);
    return var;
}

bool Dns::read16BitsRaw(uint16_t &var, const char * const data, const int &size, int &pos)
{
    if((pos+(int)sizeof(var))>size)
        return false;
    memcpy(&var,data+pos,sizeof(var));
    pos+=sizeof(var);
    return true;
}

bool Dns::read32Bits(uint32_t &var, const char * const data, const int &size, int &pos)
{
    if((pos+(int)sizeof(var))>size)
        return false;
    uint32_t t;
    memcpy(&t,data+pos,sizeof(var));
    var=be32toh(t);
    pos+=sizeof(var);
    return true;
}

bool Dns::get(Client * client, const std::string &host, const bool &https)
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
    if(host=="www.bolivia-online.com" || host=="bolivia-online.com")
    {
        std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
        char ipv6[]={0x28,0x03,0x19,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x10};
        memcpy(&targetHttp.sin6_addr,&ipv6,16);
        client->dnsRight(targetHttp);
        return true;
    }
    #endif
    if(queryListByHost.find(host)!=queryListByHost.cend())
    {
        const uint16_t &queryId=queryListByHost.at(host);
        if(queryList.find(queryId)!=queryList.cend())
        {
            if(https)
                queryList[queryId].https.push_back(client);
            else
                queryList[queryId].http.push_back(client);
            return true;
        }
        else //bug, try fix
            queryListByHost.erase(host);
    }
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    if(cache.find(host)!=cache.cend())
    {
        CacheEntry &entry=cache.at(host);
        uint64_t t=time(NULL);
        if(entry.outdated_date>t)
        {
            const uint64_t &maxTime=t+24*3600;
            //fix time drift
            if(entry.outdated_date>maxTime)
                entry.outdated_date=maxTime;
            switch(entry.status)
            {
                case StatusEntry_Right:
                    if(https)
                    {
                        #ifdef DEBUGFASTCGI
                        std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
                        #endif
                        memcpy(&targetHttps.sin6_addr,&entry.sin6_addr,16);
                        client->dnsRight(targetHttps);
                    }
                    else
                    {
                        #ifdef DEBUGFASTCGI
                        std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
                        #endif
                        memcpy(&targetHttp.sin6_addr,&entry.sin6_addr,16);
                        client->dnsRight(targetHttp);
                    }
                break;
                case StatusEntry_Error:
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                    client->dnsError();
                break;
                default:
                case StatusEntry_Wrong:
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                    client->dnsWrong();
                break;
            }
            return true;
        }
    }
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
    #endif
    if(clientInProgress>1000)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << "overloaded, clientInProgress " << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
        return false;
    }
    clientInProgress++;
    /* TODO if(isInCache())
    {load from cache}*/
    //std::cout << "dns query count merged in progress>1000" << std::endl;
    uint8_t buffer[4096];
    struct dns_query* query = (struct dns_query*)buffer;
    query->id=increment++;
    if(increment>65534)
        increment=1;
    query->flags=htobe16(288);
    query->question_count=htobe16(1);
    query->answer_count=0;
    query->authority_count=0;
    query->add_count=0;
    int pos=2+2+2+2+2+2;

    //hostname encoded
    int hostprevpos=0;
    size_t hostpos=host.find(".",hostprevpos);
    while(hostpos!=std::string::npos)
    {
        const std::string &part=host.substr(hostprevpos,hostpos-hostprevpos);
        //std::cout << part << std::endl;
        buffer[pos]=part.size();
        pos+=1;
        memcpy(buffer+pos,part.data(),part.size());
        pos+=part.size();
        hostprevpos=hostpos+1;
        hostpos=host.find(".",hostprevpos);
    }
    const std::string &part=host.substr(hostprevpos);
    //std::cout << part << std::endl;
    buffer[pos]=part.size();
    pos+=1;
    memcpy(buffer+pos,part.data(),part.size());
    pos+=part.size();

    buffer[pos]=0x00;
    pos+=1;

    //type AAAA
    buffer[pos]=0x00;
    pos+=1;
    buffer[pos]=0x1c;
    pos+=1;

    //class IN
    buffer[pos]=0x00;
    pos+=1;
    buffer[pos]=0x01;
    pos+=1;

    if(mode==Mode_IPv6)
        /*int result = */sendto(fd,&buffer,pos,0,(struct sockaddr*)&targetDnsIPv6,sizeof(targetDnsIPv6));
    else //if(mode==Mode_IPv4)
        /*int result = */sendto(fd,&buffer,pos,0,(struct sockaddr*)&targetDnsIPv4,sizeof(targetDnsIPv4));

    Query queryToPush;
    queryToPush.host=host;
    queryToPush.retryTime=0;
    queryToPush.nextRetry=time(NULL)+5;
    queryToPush.query=std::string((char *)buffer,pos);
    if(https)
        queryToPush.https.push_back(client);
    else
        queryToPush.http.push_back(client);
    addQuery(query->id,queryToPush);
    return true;
}

void Dns::addQuery(const uint16_t &id, const Query &query)
{
    queryList[id]=query;
    queryListByHost[query.host]=id;
    queryByNextDueTime[query.nextRetry].push_back(id);
}

void Dns::removeQuery(const uint16_t &id, const bool &withNextDueTime)
{
    const Query &query=queryList.at(id);
    if(withNextDueTime)
        queryByNextDueTime.erase(query.nextRetry);
    queryListByHost.erase(query.host);
    queryList.erase(id);
}

void Dns::cancelClient(Client * client,const std::string &host,const bool &https)
{
    if(queryListByHost.find(host)!=queryListByHost.cend())
    {
        const uint16_t queryId=queryListByHost.at(host);
        if(queryList.find(queryId)!=queryList.cend())
        {
            if(https)
            {
                std::vector<Client *> &httpsList=queryList[queryId].https;
                unsigned int index=0;
                while(index<httpsList.size())
                {
                    if(client==httpsList.at(index))
                    {
                        httpsList.erase(httpsList.cbegin()+index);
                        break;
                    }
                    index++;
                }
            }
            else
            {
                std::vector<Client *> &httpList=queryList[queryId].http;
                unsigned int index=0;
                while(index<httpList.size())
                {
                    if(client==httpList.at(index))
                    {
                        httpList.erase(httpList.cbegin()+index);
                        break;
                    }
                    index++;
                }
            }
            return;
        }
        else //bug, try fix
            queryListByHost.erase(host);
    }
}

int Dns::requestCountMerged()
{
    return queryListByHost.size();
}

void Dns::checkQueries()
{
    const std::map<uint64_t,std::vector<uint16_t>> queryByNextDueTime=this->queryByNextDueTime;
    for (auto const &x : queryByNextDueTime)
    {
        const uint64_t t=x.first;
        if(t>(uint64_t)time(NULL))
            return;
        const std::vector<uint16_t> &list=x.second;
        for (auto const& id : list)
        {
            Query &query=queryList.at(id);
            if(query.retryTime<2)
            {
                if(mode==Mode_IPv6)
                    /*int result = */sendto(fd,query.query.data(),query.query.size(),0,(struct sockaddr*)&targetDnsIPv6,sizeof(targetDnsIPv6));
                else //if(mode==Mode_IPv4)
                    /*int result = */sendto(fd,query.query.data(),query.query.size(),0,(struct sockaddr*)&targetDnsIPv4,sizeof(targetDnsIPv4));
                query.retryTime++;
                query.nextRetry=time(NULL)+5;
                this->queryByNextDueTime[query.nextRetry].push_back(id);
            }
            else
            {
                const std::vector<Client *> &http=query.http;
                for(Client * const c : http)
                    c->dnsError();
                const std::vector<Client *> &https=query.https;
                for(Client * const c : https)
                    c->dnsError();
                removeQuery(id);
            }

            //query=cache.erase(y);
        }
        this->queryByNextDueTime.erase(t);
        //cacheByOutdatedDate.erase(t);
    }
}
