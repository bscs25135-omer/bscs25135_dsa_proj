// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#include <cstdlib>
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack() { top = nullptr; count = 0; }
    ~Stack() { while (top) { Node *t = top; top = top->next; delete t; } }
    void push(const T &val)
    {
        if (count >= MAX_STACK_DEPTH) return;
        Node *n = new Node; n->data = val; n->next = top;
        top = n; count++;
    }
    T pop()
    {
        if (!top) return T();
        Node *n = top; T v = n->data;
        top = n->next; delete n; count--;
        return v;
    }
    T &peek() { static T none; return top ? top->data : none; }
    bool isEmpty() { return top == nullptr; }
    int32_t depth() { return count; }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int32_t i = 0;
        for (Node *n = top; n && i < maxLen; n = n->next) out[i++] = n->data;
        return i;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline() { head = tail = nullptr; stepCount = 0; }
    ~Timeline();
    void record(Snapshot *s)
    {
        TimelineNode *n = new TimelineNode;
        n->data = s; n->next = nullptr; n->prev = tail;
        if (tail) tail->next = n; else head = n;
        tail = n; stepCount++;
    }
    TimelineNode *begin() { return head; }
    int32_t getStepCount() { return stepCount; }

};

// Core structs
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

Timeline::~Timeline()
{
    while (head) { 
        TimelineNode *n = head;
        head = head->next;
        delete n->data;
        delete n;
    }
}

struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// resolve.bin - bookkeeping
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

bool isWs(char c)
{
    return c == ' ' or c == '\t' or c == '\r' or c == '\n';
}

static string trim(const string &s)
{
    int32_t a = 0;
    int32_t b = (int32_t)s.size() - 1;

    while (a <= b and (s[a] == ' ' or s[a] == '\t' or s[a] == '\r' or s[a] == '\n'))
        a++;

    while (b >= a and (s[b] == ' ' or s[b] == '\t' or s[b] == '\r' or s[b] == '\n'))
        b--;

    string out = "";
    for (int32_t i = a; i <= b; i++)
        out += s[i];
    return out;
}

// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    string l;
    while (getline(in, l))
    {
        l = trim(l);
        if (l.size() == 0)
            continue;
        if (l.size() >= 2 && l[0] == '/' && l[1] == '/')
            continue;
        out = l;
        return true;
    }
    return false;
}
string firstWord(const string &line)
{
    int32_t i = 0, n = (int32_t)line.size();
    while (i < n && isWs(line[i]))
        i++;
    string w = "";
    while (i < n && !isWs(line[i]))
    {
        w += line[i];
        i++;
    }
    return w;
}
string secondWord(const string &line)
{
    int32_t i = 0, n = (int32_t)line.size();
    while (i < n and isWs(line[i]))
        i++;
    while (i < n and !isWs(line[i]))
        i++;
    while (i < n and isWs(line[i]))
        i++;
    string w = "";
    while (i < n and !isWs(line[i]))
    {
        w += line[i];
        i++;
    }
    return w;
}

bool validateProgram(const char *sourcePath)
{
    ifstream in(sourcePath);
    if (!in)
    {
        cerr << "error: cannot open " << sourcePath << "\n";
        return false;
    }
    string l;
    bool open = false;
    while (readSourceLine(in, l))
    {
        string w = firstWord(l);
        if (w == "func")
        {
            if (open)
            {
                cerr << "error: nested func declaration -> " << l << "\n";
                return false;
            }
            open = true;
        }
        else if (w == "func_end")
        {
            if (!open)
            {
                cerr << "error: func_end without a matching func\n";
                return false;
            }
            open = false;
        }
    }
    if (open)
    {
        cerr << "error: func without a matching func_end\n";
        return false;
    }
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    int64_t pos = ftell(f);
    int32_t sz = (int32_t)text.size();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&sz, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < sz; i++)
        fputc(text[i], f);
    return pos;
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t off;
    int32_t sz;
    if (fread(&off, sizeof(int64_t), 1, f) != 1)
        return -1;
    if (fread(&sz, sizeof(int32_t), 1, f) != 1)
        return -1;
    outText = "";
    for (int32_t i = 0; i < sz; i++)
    {
        int c = fgetc(f);
        if (c == EOF)
            return -1;
        outText += (char)c;
    }
    return off;
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 

ifstream in(sourcePath);
    if (!in)
    {
        cerr << "error: cannot open " << sourcePath << "\n";
        return -3;
    }
    FILE *f = fopen(resolveBinPath, "wb+");
    if (!f)
    {
        cerr << "error: cannot create " << resolveBinPath << "\n";
        return -3;
    }

    string l;
    while (readSourceLine(in, l))
    {
        string w = firstWord(l);
        int64_t pos = writeResolveRecord(f, ftell(f), l);
        if (w == "func")
        {
            string name = secondWord(l);
            for (int32_t i = 0; i < funcCount; i++)
            {
                if (funcArray[i].funcName == name)
                {
                    cerr << "error: function '" << name << "' defined twice\n";
                    fclose(f);
                    return -3;
                }
            }
            if (funcCount >= MAX_FUNCS)
            {
                cerr << "error: too many functions\n";
                fclose(f);
                return -3;
            }
            funcArray[funcCount].funcName = name;
            funcArray[funcCount].byteOffsetInResolveBin = pos;
            funcCount++;
        }
        else if (w == "call")
        {
            if (patchCount >= MAX_PATCHES)
            {
                cerr << "error: too many calls\n";
                fclose(f);
                return -3;
            }
            patches[patchCount].byteOffsetOfOffsetField = pos;
            patches[patchCount].targetFuncName = secondWord(l);
            patchCount++;
        }
    }

    for (int32_t i = 0; i < patchCount; i++)
    {
        int64_t target = -1;
        for (int32_t j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
            {
                target = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }
        if (target < 0)
        {
            cerr << "error: call to undefined function '" << patches[i].targetFuncName << "'\n";
            fclose(f);
            return -2;
        }
        fseek(f, (long)patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&target, sizeof(int64_t), 1, f);
    }
    fclose(f);

    for (int32_t i = 0; i < funcCount; i++)
    {
        if (funcArray[i].funcName == "main")
            return funcArray[i].byteOffsetInResolveBin;
    }
    cerr << "error: no main function\n";
    return -1;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
    int32_t n = 0, i = 0, len = (int32_t)line.size();
    while (n < maxTokens)
    {
        while (i < len and isWs(line[i]))
            i++;
        if (i >= len)
            break;
        string w = "";
        while (i < len and !isWs(line[i]))
        {
            w += line[i];
            i++;
        }
        tokens[n].text = w;
        if (n == 0)
            tokens[n].type = KEYWORD;
        else if (n == 1)
            tokens[n].type = IDENTIFIER;
        else
            tokens[n].type = PARAM;
        n++;
    }
    return n;

}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
    Snapshot *s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}

bool isNum(const string &s)
{
    int32_t n = (int32_t)s.size();
    if (n == 0)
        return false;
    int32_t i = 0;
    if (s[0] == '-' or s[0] == '+')
        i = 1;
    if (i >= n)
        return false;
    for (; i < n; i++)
    {
        if (s[i] < '0' or s[i] > '9')
            return false;
    }
    return true;
}
int32_t toInt(const string &s)   // replaces strtol; call only after isNum()
{
    int32_t i = 0, sign = 1, v = 0;
    if (s[0] == '-')
    {
        sign = -1;
        i = 1;
    }
    else if (s[0] == '+')
        i = 1;
    for (; i < (int32_t)s.size(); i++)
        v = v * 10 + (s[i] - '0');
    return v * sign;
}
Variable *findVar(Frame &fr, const string &n)
{
    for (int32_t i = 0; i < fr.argc; i++)
    {
        if (fr.argv[i].name == n)
            return &fr.argv[i];
    }
    for (int32_t i = 0; i < fr.localCount; i++)
    {
        if (fr.locals[i].name == n)
            return &fr.locals[i];
    }
    return nullptr;
}
Variable *getVar(Frame &fr, const string &n)   // find, or create as a local = 0
{
    Variable *v = findVar(fr, n);
    if (v != nullptr)
        return v;
    if (fr.localCount >= MAX_VARS_PER_FRAME)
    {
        cerr << "error: too many locals in '" << fr.func_name << "'\n";
        return nullptr;
    }
    v = &fr.locals[fr.localCount];
    fr.localCount++;
    v->name = n;
    v->value = 0;
    return v;
}
int32_t getVal(Frame &fr, const string &s)     // literal, or a variable's value
{
    if (isNum(s))
        return toInt(s);
    Variable *v = findVar(fr, s);
    if (v != nullptr)
        return v->value;
    return 0;
}


void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    FILE *f = fopen(resolveBinPath, "rb");
    if (not f)
    {
        cerr << "error: cannot open " << resolveBinPath << "\n";
        return;
    }
    Stack<Frame> st;
    Frame m;
    m.func_name = "main";
    m.argc = 0;
    m.returnLine = -1;
    m.localCount = 0;
    st.push(m);
    string l;
    Token t[MAX_TOKENS];
    fseek(f, (long)mainOffset, SEEK_SET);
    readResolveRecord(f, l);
    while (not st.isEmpty())
    {
        int64_t pos = ftell(f);
        int64_t off = readResolveRecord(f, l);
        if (off < 0)
            break;
        int32_t nt = tokenizeLine(l, t, MAX_TOKENS);
        if (nt == 0)
            continue;
        string kw = t[0].text;
        if (kw == "call")
        {
            if (st.depth() >= MAX_STACK_DEPTH)
            {
                cerr << "error: stack overflow\n";
                break;
            }
            Frame *cur = &st.peek();
            string hl;
            Token h[MAX_TOKENS];
            fseek(f, (long)off, SEEK_SET);
            readResolveRecord(f, hl);
            int32_t hn = tokenizeLine(hl, h, MAX_TOKENS);
            Frame fr;
            fr.func_name = "";
            if (hn > 1)
                fr.func_name = h[1].text;
            fr.argc = 0;
            if (hn > 2)
                fr.argc = hn - 2;
            for (int32_t i = 0; i < fr.argc; i++)
            {
                fr.argv[i].name = h[i + 2].text;
                fr.argv[i].value = 0;
                if (i + 2 < nt)
                    fr.argv[i].value = getVal(*cur, t[i + 2].text);
            }
            fr.returnLine = (int32_t)pos;
            fr.localCount = 0;
            st.push(fr);
        }
        else if (kw == "func_end")
        {
            if (st.depth() == 1)
            {
                timeline.record(buildSnapshot(st));
                break;
            }
            Frame fr = st.pop();
            string cl;
            Token c[MAX_TOKENS];
            fseek(f, (long)fr.returnLine, SEEK_SET);
            readResolveRecord(f, cl);
            int32_t cn = tokenizeLine(cl, c, MAX_TOKENS);
            Frame &up = st.peek();
            for (int32_t i = 0; i < fr.argc and i + 2 < cn; i++)
            {
                if (isNum(c[i + 2].text))
                    continue;
                Variable *v = getVar(up, c[i + 2].text);
                if (v != nullptr)
                    v->value = fr.argv[i].value;
            }
        }
        else if (kw == "set" or kw == "add" or kw == "sub" or kw == "mul" or kw == "div")
        {
            Frame *cur = &st.peek();
            if (nt < 3)
                cerr << "error: '" << l << "' needs a target and an operand\n";
            else
            {
                int32_t y = getVal(*cur, t[2].text);
                Variable *v = getVar(*cur, t[1].text);
                if (v != nullptr)
                {
                    int64_t x = v->value;
                    int64_t r = x;
                    if (kw == "set")
                        r = y;
                    else if (kw == "add")
                        r = x + y;
                    else if (kw == "sub")
                        r = x - y;
                    else if (kw == "mul")
                        r = x * y;
                    else if (y == 0)
                        cerr << "error: division by zero (skipped)\n";
                    else
                        r = x / y;
                    v->value = (int32_t)r;
                }
            }
        }
        else
            cerr << "warning: unknown instruction '" << kw << "'\n";
        timeline.record(buildSnapshot(st));
    }
    fclose(f);
}

void wstr(FILE *f, const string &s)
{
    int32_t n = (int32_t)s.size();
    fwrite(&n, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < n; i++)
        fputc(s[i], f);
}
void wvar(FILE *f, const Variable &v)
{
    wstr(f, v.name);
    fwrite(&v.value, sizeof(int32_t), 1, f);
}
void wframe(FILE *f, const Frame &fr)
{
    wstr(f, fr.func_name);
    fwrite(&fr.argc, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < fr.argc; i++)
        wvar(f, fr.argv[i]);
    fwrite(&fr.returnLine, sizeof(int32_t), 1, f);
    fwrite(&fr.localCount, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < fr.localCount; i++)
        wvar(f, fr.locals[i]);
}
void wsnap(FILE *f, const Snapshot &s)
{
    fwrite(&s.stackDepth, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < s.stackDepth; i++)
        wframe(f, s.callStack[i]);
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
FILE *f = fopen(tdbgPath, "wb");
if (not f)
{
    cerr << "error: cannot create " << tdbgPath << "\n";
    return;
}
TTDBHeader h;
h.magic[0] = 'T';
h.magic[1] = 'T';
h.magic[2] = 'D';
h.magic[3] = 'B';
h.version = 1;
h.stepCount = 0;
h.indexOffset = 0;
writeHeader(f, h);
int32_t n = timeline.getStepCount();
int64_t *idx = new int64_t[n + 1];
int32_t i = 0;
TimelineNode *x = timeline.begin();
while (x != nullptr and i < n)
{
    idx[i] = ftell(f);
    wsnap(f, *x->data);
    i++;
    x = x->next;
}
h.stepCount = i;
h.indexOffset = ftell(f);
fwrite(idx, sizeof(int64_t), i, f);
fseek(f, 0, SEEK_SET);
writeHeader(f, h);
fclose(f);
delete[] idx;
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    if (mainOffset < 0)
    {
        return 1;
    }

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}