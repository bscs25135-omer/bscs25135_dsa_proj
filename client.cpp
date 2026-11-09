// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;

// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;   // false until a "start" has worked
    TdbgReader reader; // the .tdbg file stays open for the whole session
    int64_t currentStep;
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.

    size_t got = 0;
    while (got < n)
    {
        ssize_t r = recv(sock, buf + got, n - got, 0);
        if (r <= 0)
            return false;
        got = got + r;
    }
    return true;
}
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.

    size_t sent = 0;
    while (sent < n)
    {
        ssize_t r = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0)
            return false;
        sent = sent + r;
    }
    return true;
}

bool parseIPv4(const char *text, unsigned char out[4])
{
    int part = 0;
    int value = 0;
    int digits = 0;
    for (int i = 0;; i++)
    {
        char c = text[i];
        if (c >= '0' and c <= '9')
        {
            value = value * 10 + (c - '0');
            digits++;
            if (digits > 3 or value > 255)
                return false;
        }
        else if (c == '.' or c == '\0')
        {
            if (digits == 0)
                return false;
            if (part >= 4)
                return false;
            out[part] = (unsigned char)value;
            part++;
            value = 0;
            digits = 0;
            if (c == '\0')
                break;
        }
        else
            return false;
    }
    return part == 4;
}

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.

    unsigned char ip[4];
    if (port < 1 or port > 65535 or !parseIPv4(host, ip))
        return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    sockaddr addr;
    memset(&addr, 0, sizeof(addr));
    addr.sa_family = AF_INET;
    addr.sa_data[0] = (char)((port >> 8) & 0xFF);
    addr.sa_data[1] = (char)(port & 0xFF);
    for (int i = 0; i < 4; i++)
        addr.sa_data[2 + i] = (char)ip[i];
    if (connect(s, &addr, sizeof(addr)) < 0)
    {
        close(s);
        return -1;
    }
    return s;
}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.

    FILE *f = fopen(sourcePath, "rb");
    if (!f)
        return false;
    char buf[IO_BUFFER_SIZE];
    uint32_t crc = 0xFFFFFFFF;
    while (true)
    {
        size_t n = fread(buf, 1, IO_BUFFER_SIZE, f);
        if (n == 0)
            break;
        crc = crcUpdate(crc, buf, n);
        if (!sendAll(sock, buf, n))
        {
            fclose(f);
            return false;
        }
    }
    fclose(f);
    char t[4];
    putU32(t, crc ^ 0xFFFFFFFF);
    if (!sendAll(sock, t, 4))
        return false;
    shutdown(sock, SHUT_WR);
    return true;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.

    TrailerChecker chk;
    char buf[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE];
    FILE *fp = nullptr;
    int status = -1;
    bool gotStatus = false;
    outError = "";

    while (true)
    {
        ssize_t n = recv(sock, buf, IO_BUFFER_SIZE, 0);
        if (n <= 0)
            break;
        int32_t r = chk.feed(buf, (int32_t)n, out);
        int32_t start = 0;
        if (!gotStatus and r > 0)
        {
            status = (unsigned char)out[0];
            gotStatus = true;
            start = 1;
            if (status == 0)
            {
                fp = fopen(outTdbgPath, "wb");
                if (!fp)
                {
                    outError = "cannot create the file " + string(outTdbgPath);
                    return false;
                }
            }
        }
        if (status == 0 and fp != nullptr)
            fwrite(out + start, 1, r - start, fp);
        else if (status == 1)
        {
            for (int32_t i = start; i < r; i++)
                outError += out[i];
        }
    }
    if (fp != nullptr)
        fclose(fp);

    if (chk.tailLen == 0)
    {
        outError = "no reply from the server";
        return false;
    }
    if (!gotStatus or !chk.verify())
    {
        if (status == 0)
            remove(outTdbgPath);
        outError = "checksum mismatch, response corrupted";
        return false;
    }
    if (status == 0)
        return true;
    if (status != 1)
    {
        outError = "unknown reply from the server";
        return false;
    }
    return false;
}

bool requestDebugFile(const char *host, int port, const char *sourcePath, const char *outPath, string &outError)
{
    FILE *t = fopen(sourcePath, "rb"); // check the file first so the error is clear
    if (!t)
    {
        outError = "cannot open the file " + string(sourcePath);
        return false;
    }
    fclose(t);

    int sock = connectToHost(host, port);
    if (sock < 0)
    {
        outError = "cannot connect to the load balancer at " + string(host) + ":" + to_string(port);
        return false;
    }
    if (!uploadSource(sock, sourcePath))
    {
        // the load balancer might have refused are request("server busy") so try reading its message
        string e2;
        receiveResponse(sock, outPath, e2);
        remove(outPath);
        if (e2.size() > 0 and e2 != "no reply from the server")
            outError = e2;
        else
            outError = "failed while sending the file";
        close(sock);
        return false;
    }
    bool ok = receiveResponse(sock, outPath, outError);
    close(sock);
    return ok;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

/////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////

bool readExact(int fd, void *buf, size_t n)
{
    char *p = (char *)buf;
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0)
            return false;
        got = got + r;
    }
    return true;
}
bool readInt(int fd, int32_t &v)
{
    return readExact(fd, &v, sizeof(int32_t));
}
bool readStr(int fd, string &out)
{
    const int32_t maxLen = 4096;
    int32_t len;
    if (!readInt(fd, len))
        return false;
    if (len < 0 or len > maxLen)
        return false;
    char tmp[maxLen + 1];
    if (!readExact(fd, tmp, len))
        return false;
    out = "";
    for (int32_t i = 0; i < len; i++)
        out += tmp[i];
    return true;
}
bool readVar(int fd, Variable &v)
{
    if (!readStr(fd, v.name))
        return false;
    return readInt(fd, v.value);
}
bool readFrame(int fd, Frame &fr)
{
    if (!readStr(fd, fr.func_name))
        return false;
    if (!readInt(fd, fr.argc))
        return false;
    if (fr.argc < 0 or fr.argc > MAX_VARS_PER_FRAME)
        return false;
    for (int32_t i = 0; i < fr.argc; i++)
    {
        if (!readVar(fd, fr.argv[i]))
            return false;
    }
    if (!readInt(fd, fr.returnLine))
        return false;
    if (!readInt(fd, fr.localCount))
        return false;
    if (fr.localCount < 0 or fr.localCount > MAX_VARS_PER_FRAME)
        return false;
    for (int32_t i = 0; i < fr.localCount; i++)
    {
        if (!readVar(fd, fr.locals[i]))
            return false;
    }
    return true;
}

/////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////

bool openTdbg(const char *path, TdbgReader &r)
{
    r.file_index = -1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    int fd = dup(fileno(f));
    fclose(f);
    if (fd < 0)
        return false;

    TTDBHeader h;
    if (!readExact(fd, h.magic, 4) or !readInt(fd, h.version) or !readInt(fd, h.stepCount) or !readExact(fd, &h.indexOffset, sizeof(int64_t)))
    {
        close(fd);
        return false;
    }
    if (h.magic[0] != 'T' or h.magic[1] != 'T' or h.magic[2] != 'D' or h.magic[3] != 'B')
    {
        close(fd);
        return false;
    }
    off_t fileSize = lseek(fd, 0, SEEK_END);
    if (h.stepCount <= 0 or h.indexOffset < 20 or h.indexOffset + (int64_t)h.stepCount * 8 > fileSize)
    {
        close(fd);
        return false;
    }

    vector<int64_t> idx;
    if (lseek(fd, (off_t)h.indexOffset, SEEK_SET) < 0)
    {
        close(fd);
        return false;
    }
    for (int32_t i = 0; i < h.stepCount; i++)
    {
        int64_t o;
        if (!readExact(fd, &o, sizeof(int64_t)) or o < 20 or o >= h.indexOffset)
        {
            close(fd);
            return false;
        }
        idx.push_back(o);
    }
    r.file_index = fd;
    r.header = h;
    r.index = idx;
    return true;
}
void closeTdbg(TdbgReader &r)
{
    if (r.file_index >= 0)
        close((int)r.file_index);
    r.file_index = -1;
    vector<int64_t>().swap(r.index);
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (r.file_index < 0 or step < 0 or step >= (int32_t)r.index.size())
        return nullptr;
    int fd = (int)r.file_index;
    if (lseek(fd, (off_t)r.index[step], SEEK_SET) < 0)
        return nullptr;

    Snapshot *s = new Snapshot();
    if (!readInt(fd, s->stackDepth) or s->stackDepth < 0 or s->stackDepth > MAX_STACK_DEPTH)
    {
        delete s;
        return nullptr;
    }
    for (int32_t i = 0; i < s->stackDepth; i++)
    {
        if (!readFrame(fd, s->callStack[i]))
        {
            delete s;
            return nullptr;
        }
    }
    return s;
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active_flag = 0;
    s.reader.file_index = -1;
    s.reader.header.version = 0;
    s.reader.header.stepCount = 0;
    s.reader.header.indexOffset = 0;
    s.currentStep = 0;
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active_flag)
        closeTdbg(s.reader);
    s.active_flag = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.

    Snapshot *snap = readSnapshotAt(s.reader, (int32_t)s.currentStep);
    if (snap == nullptr)
    {
        cout << "damaged .tdbg file" << endl;
        return false;
    }
    printSnapshot((int32_t)s.currentStep + 1, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    if (!s.active_flag)
    {
        cout << "no session yet, use: start <file>" << endl;
        return false;
    }
    int64_t target = s.currentStep + (int64_t)delta;
    if (target < 0 or target >= s.reader.header.stepCount)
    {
        cout << "out of range: valid steps are 1 to " << s.reader.header.stepCount
             << " (you are at " << s.currentStep + 1 << ")" << endl;
        return false;
    }
    s.currentStep = target;
    return showCurrent(s);
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).

    string tmp = string(TDBG_FILE) + ".part";
    string err;
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), tmp.c_str(), err))
    {
        cout << err << endl;
        return false;
    }
    TdbgReader fresh;
    fresh.file_index = -1;
    if (!openTdbg(tmp.c_str(), fresh))
    {
        cout << "server sent a damaged .tdbg file" << endl;
        remove(tmp.c_str());
        return false;
    }
    rename(tmp.c_str(), TDBG_FILE);

    if (s.active_flag)
        closeTdbg(s.reader);
    s.reader = fresh;
    s.currentStep = 0;
    s.active_flag = 1;
    return showCurrent(s);
}
bool cmdNext(DebugSession &s) { return moveBy(s, 1); }
bool cmdPrv(DebugSession &s) { return moveBy(s, -1); }
bool cmdJmpNext(DebugSession &s, int32_t n) { return moveBy(s, n); }
bool cmdJmpPrv(DebugSession &s, int32_t n) { return moveBy(s, -n); }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "\n--- step " << step << " of " << totalSteps << " ---\n";
    cout << "call stack:\n";

    for (int32_t i = 0; i < s.stackDepth; i++)
    {
        const Frame &f = s.callStack[i];

        if (i == 0)
        {
            cout << " > ";
        }
        else
        {
            cout << "   ";
        }

        cout << f.func_name << "(";
        for (int32_t j = 0; j < f.argc; j++)
        {
            if (j > 0)
            {
                cout << ", ";
            }
            cout << f.argv[j].name << " = " << f.argv[j].value;
        }
        cout << ")";

        if (f.localCount > 0)
        {
            cout << " (locals: ";
            for (int32_t j = 0; j < f.localCount; j++)
            {
                if (j > 0)
                {
                    cout << ", ";
                }
                cout << f.locals[j].name << " = " << f.locals[j].value;
            }
            cout << ")";
        }
        cout << "\n";
    }
}
void printHelp()
{

    cout << "\ncommands:\n"
         << "  start <file>     run program on server\n"
         << "  next / prv       step forward / backward\n"
         << "  jmp next/prv <n> jump n steps forward / backward\n"
         << "  help             show this message\n"
         << "  quit, exit       exit\n\n";
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function.

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.
}
