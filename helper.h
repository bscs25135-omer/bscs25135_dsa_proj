#pragma once
#include <string>
#include <cstdint>
#include <cstddef>
using namespace std;


// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

uint32_t crcUpdate(uint32_t crc, const char *buf, size_t n) //this is the main func for checksum in which we take xor for difference
{                                                           // and process the bytes and update the crc
    for (size_t i = 0; i < n; i++)
    {
        crc = crc ^ (unsigned char)buf[i];
        for (int j = 0; j < 8; j++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc = crc >> 1;
        }
    }
    return crc;
}

uint32_t crcOf(const char *buf, size_t n)
{
    return crcUpdate(0xFFFFFFFF, buf, n) ^ 0xFFFFFFFF;
}

void putU32(char out[4], uint32_t v) // takes 32 bit int and breaks it onto 4 diff parts
{
    out[0] = (char)((v >> 24) & 0xFF);
    out[1] = (char)((v >> 16) & 0xFF);
    out[2] = (char)((v >> 8) & 0xFF);
    out[3] = (char)(v & 0xFF);
}

uint32_t getU32(const unsigned char in[4]) // this combines it back(24, 16, 8 are shifting factors hence in[32] stays as is)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}// | is bitwise or

struct TrailerChecker
{
    uint32_t crc;
    unsigned char tail[4];
    int32_t tailLen;
 
    TrailerChecker()
    {
        crc = 0xFFFFFFFF;
        tailLen = 0;
    }

    int32_t feed(const char *buf, int32_t n, char *out)
    {
        int32_t released = 0;
        for (int32_t i = 0; i < n; i++)
        {
            if (tailLen < 4)
            {
                tail[tailLen] = (unsigned char)buf[i];
                tailLen++;
            }
            else
            {
                char r = (char)tail[0]; //r is released meaning removed from tail yet stored in r
                tail[0] = tail[1];
                tail[1] = tail[2];
                tail[2] = tail[3];
                tail[3] = (unsigned char)buf[i];
                crc = crcUpdate(crc, &r, 1);
                if (out != nullptr)
                    out[released] = r;
                released++;
            }
        }
        return released;
    }
    bool verify()
    {
        if (tailLen < 4)
            return false;
        return getU32(tail) == (crc ^ 0xFFFFFFFF);
    }
};