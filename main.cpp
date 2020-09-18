#include <errno.h>
#include <sys/epoll.h>
#include "Server.hpp"
#include "Client.hpp"
#include "Http.hpp"
#include "Dns.hpp"
#include "Backend.hpp"
#include "Cache.hpp"
#include "Timer.hpp"
#include "Timer/DNSCache.hpp"
#include "Timer/DNSQuery.hpp"
#include <vector>
#include <cstring>
#include <cstdio>
#include <signal.h>
#include <iostream>
#include <sys/stat.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_EVENTS 1024

void signal_callback_handler(int signum) {
    printf("Caught signal SIGPIPE %d\n",signum);
}

int main(int argc, char *argv[])
{
    /* Catch Signal Handler SIGPIPE */
    if(signal(SIGPIPE, signal_callback_handler)==SIG_ERR)
    {
        std::cerr << "signal(SIGPIPE, signal_callback_handler)==SIG_ERR, errno: " << std::to_string(errno) << std::endl;
        abort();
    }
    mkdir("cache", S_IRWXU);


    std::vector <std::string> sources;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--flatcache") {
            Cache::hostsubfolder=false;
        }/* else if (std::string(argv[i]) == "--maxiumSizeToBeSmallFile") {
            if (i + 1 < argc) { // Make sure we aren't at the end of argv!
                std::string maxiumSizeToBeSmallFile = argv[i++]; // Increment 'i' so we don't get the argument as the next argv[i].

            } else { // Uh-oh, there was no argument to the destination option.
                std::cerr << "--maxiumSizeToBeSmallFile option requires one argument." << std::endl;
                return 1;
            }
        } else if (std::string(argv[i]) == "--maxiumSmallFileCacheSize") {
            if (i + 1 < argc) { // Make sure we aren't at the end of argv!
                std::string maxiumSmallFileCacheSize = argv[i++]; // Increment 'i' so we don't get the argument as the next argv[i].

            } else { // Uh-oh, there was no argument to the destination option.
                std::cerr << "--memorycachemap option requires one argument." << std::endl;
                return 1;
            }
        }*/ else if (std::string(argv[i]) == "--help") {
            std::cerr << "--flatcache: use flat cache, the host cache folder is not created" << std::endl;
            //std::cerr << "--maxiumSizeToBeSmallFile: (TODO) if smaller than this size, save into memory" << std::endl;
            //std::cerr << "--maxiumSmallFileCacheSize: (TODO) The maximum content stored in memory, this cache prevent syscall and disk seek" << std::endl;
            return 1;
        } else {
            sources.push_back(argv[i]);
        }
    }

    (void)argc;
    (void)argv;

    //the event loop
    struct epoll_event ev, events[MAX_EVENTS];
    memset(&ev,0,sizeof(ev));
    int nfds, epollfd;

    Http::fdRandom=open("/dev/urandom",O_RDONLY);

    ev.events = EPOLLIN|EPOLLET;

    if ((epollfd = epoll_create1(0)) == -1) {
        printf("epoll_create1: %s", strerror(errno));
        return -1;
    }
    EpollObject::epollfd=epollfd;
    Dns::dns=new Dns();
    DNSCache dnsCache;
    dnsCache.start(3600*1000);
    DNSQuery dnsQuery;
    dnsQuery.start(10);

    /* cachePath (content header, 64Bits aligned):
     * 64Bits: access time
     * 64Bits: last modification time check
     * 64Bits: modification time */

    /*Server *server=*///new Server("/run/fastcgicdn.sock");
    Server s("fastcgicdn.sock");
    (void)s;
    std::vector<Client *> newDeleteClient,oldDeleteClient;
    std::vector<Backend *> newDeleteBackend,oldDeleteBackend;
    for (;;) {
        if ((nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1)) == -1)
            printf("epoll_wait error %s", strerror(errno));
        for(Client * client : oldDeleteClient)
            delete client;
        for(Backend * b : oldDeleteBackend)
            delete b;
        oldDeleteClient=newDeleteClient;
        newDeleteClient.clear();
        oldDeleteBackend=newDeleteBackend;
        newDeleteBackend.clear();
        for (int n = 0; n < nfds; ++n)
        {
            epoll_event &e=events[n];
            switch(static_cast<EpollObject *>(e.data.ptr)->getKind())
            {
                case EpollObject::Kind::Kind_Server:
                {
                    Server * server=static_cast<Server *>(e.data.ptr);
                    server->parseEvent(e);
                }
                break;
                case EpollObject::Kind::Kind_Client:
                {
                    Client * client=static_cast<Client *>(e.data.ptr);
                    client->parseEvent(e);
                    if(!client->isValid())
                    {
                        //if(!deleteClient.empty() && deleteClient.back()!=client)
                        newDeleteClient.push_back(client);
                        client->disconnect();
                    }
                }
                break;
                case EpollObject::Kind::Kind_Backend:
                {
                    Backend * backend=static_cast<Backend *>(e.data.ptr);
                    backend->parseEvent(e);
                    /*if(!http->toRemove.empty())
                        newDeleteHttp.insert(newDeleteHttp.end(),http->toRemove.cbegin(),http->toRemove.cend());*/
                    if(!backend->isValid())
                        newDeleteBackend.push_back(backend);
                }
                break;
                case EpollObject::Kind::Kind_Dns:
                {
                    Dns * dns=static_cast<Dns *>(e.data.ptr);
                    dns->parseEvent(e);
                }
                break;
                case EpollObject::Kind::Kind_Timer:
                {
                    static_cast<Timer *>(e.data.ptr)->exec();
                    static_cast<Timer *>(e.data.ptr)->validateTheTimer();
                }
                break;
                default:
                break;
            }
        }
    }

    return 0;
}
