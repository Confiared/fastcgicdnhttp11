QT -= gui

CONFIG += c++11 console
CONFIG -= app_bundle

# The following define makes your compiler emit warnings if you use
# any feature of Qt which as been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0
#DEFINES += HTTPGZIP
contains(DEFINES, HTTPGZIP) {
LIBS += -lz
}

DEFINES += DEBUGHTTPS
LIBS += -lssl -lcrypto

CONFIG(debug, debug|release) {
DEFINES += DEBUGFASTCGI
DEFINES += DEBUGDNS
}

SOURCES += main.cpp \
    Backend.cpp \
    Client.cpp \
    Common.cpp \
    EpollObject.cpp \
    Http.cpp \
    Https.cpp \
    Server.cpp \
    Dns.cpp \
    Timer.cpp \
    Cache.cpp \
    Timer/DNSCache.cpp \
    Timer/DNSQuery.cpp

HEADERS += \
    Backend.hpp \
    Client.hpp \
    Common.hpp \
    EpollObject.hpp \
    Http.hpp \
    Https.hpp \
    Server.hpp \
    Dns.hpp \
    Timer.hpp \
    Cache.hpp \
    Timer/DNSCache.hpp \
    Timer/DNSQuery.hpp
