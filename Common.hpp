#ifndef COMMON_H
#define COMMON_H

#include <string>

class Common
{
public:
    Common();
    static inline uint8_t hexToDecUnit(const char& c, bool &ok);
    static std::string hexaToBinary(const std::string &hexa);
    static std::string binarytoHexa(const char * const data, const uint32_t &size);
    static uint64_t hexaTo64Bits(const std::string &hexa);
};

#endif // COMMON_H
