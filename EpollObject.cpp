#include "EpollObject.hpp"

int EpollObject::epollfd;

EpollObject::EpollObject() :
    fd(-1),
    kind(Kind_Server)
{
}

EpollObject::~EpollObject()
{
}

const EpollObject::Kind &EpollObject::getKind() const
{
    return kind;
}

bool EpollObject::isValid() const
{
    return fd!=-1;
}
