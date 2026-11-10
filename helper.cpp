#include "helper.h"

// this is the main func for checksum: take xor for difference, then process
// the bytes and update the crc
uint32_t crcUpdate(uint32_t crc, const char *buf, size_t n)
{
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

// takes 32-bit int and breaks it into 4 bytes (big-endian)
void putU32(char out[4], uint32_t v)
{
    out[0] = (char)((v >> 24) & 0xFF);
    out[1] = (char)((v >> 16) & 0xFF);
    out[2] = (char)((v >> 8) & 0xFF);
    out[3] = (char)(v & 0xFF);
}

// combines 4 bytes back into a 32-bit int (big-endian)
uint32_t getU32(const unsigned char in[4])
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}