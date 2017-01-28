/*
MIT License

Copyright (c) 2017 Scott Petersen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// sampling profiler for lua
// samples approx 1000 lua frames/s

#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>

#define CURRENTLINE 0 // record executing line # (vs executing function's definition line #)
#define STACK 1 // sample entire stack (vs just top frame)
#define CALLCHECK 0 // check time at call hooks to mitigate i/s dropping drastically (at cost of some overhead)

#if CALLCHECK
#define HOOKMASK (LUA_MASKCOUNT|LUA_MASKCALL)
#else
#define HOOKMASK (LUA_MASKCOUNT)
#endif

static uint64_t now()
{
  struct timeval tv;

  gettimeofday(&tv, NULL);

  return (uint64_t)1000000 * tv.tv_sec + tv.tv_usec;
}

static int gLastCount = 1; // previous hook count setting
static uint64_t gLastTime; // last time we started running code

#define WINSIZE 19

// windowed median for instructions per millisecond
static int gIPMSWin[WINSIZE] = { 0 };
static int gIPMSRing[WINSIZE] = { 0 };
static int gIPMSRingIdx = 0;

#define LLFIND(KEY) \
  while(cur) \
  { \
    if(cur->KEY == KEY) \
    { \
      *prevNext = cur->next; \
      cur->next = *head; \
      *head = cur; \
      return cur; \
    } \
    prevNext = &cur->next; \
    cur = cur->next; \
  }

// LL of line #s
struct Line
{
  struct Line *next;
  int line;
  uint64_t count; // how many times hit
#if STACK
  struct Source *callHead;
#endif
};

// get the given line # from the linked list or create it
static struct Line *getLine(struct Line **head, int line)
{
  struct Line **prevNext = head;
  struct Line *cur = *head;

  LLFIND(line)

  cur = (struct Line *)malloc(sizeof(struct Line));

  cur->next = *head;
  cur->line = line;
  cur->count = 0;
#if STACK
  cur->callHead = NULL;
#endif
  *head = cur;
  return cur;
}

// LL of sources
static struct Source
{
  struct Source *next;
  const char *source;
  struct Line *lineHead;
} *gSourceHead = NULL;

// get the given source from the linked list or create it
static struct Source *getSource(struct Source **head, const char *source)
{
  struct Source **prevNext = head;
  struct Source *cur = *head;

  LLFIND(source)

  cur = (struct Source *)malloc(sizeof(struct Source));

  cur->next = *head;
  cur->source = source;
  cur->lineHead = NULL;
  *head = cur;
  return cur;
}

static struct Line *getARLine(lua_State *L, lua_Debug *ar, int depth)
{
#if STACK
  struct Source **head;

  {
    lua_Debug ar1;

    if(lua_getstack(L, depth + 1, &ar1))
    {
      struct Line *line = getARLine(L, &ar1, depth + 1);

      head = &line->callHead;
    }
    else
      head = &gSourceHead;
  }
#else
  struct Source **head = &gSourceHead;
#endif

#if CURRENTLINE
  lua_getinfo(L, "Sl", ar);

  int lineNo = ar->currentline;
#else
  lua_getinfo(L, "S", ar);

  int lineNo = ar->linedefined;
#endif

  struct Source *source = getSource(head, ar->source);

  return getLine(&source->lineHead, lineNo); 
}

// update the median window / ring buffer with a new value
// return new median
static int updateIPMSWin(int ipms)
{
  // find next index in ring buffer
  if(++gIPMSRingIdx >= WINSIZE)
    gIPMSRingIdx = 0;

  // remove this entry from median window
  int ipmsRem = gIPMSRing[gIPMSRingIdx];

  // replace it in the ring
  gIPMSRing[gIPMSRingIdx] = ipms;

  int i = 0;

  // find it in the med window
  while(gIPMSWin[i] != ipmsRem)
    i++;

  // it was less, bubble up to repair
  if(ipms < ipmsRem)
  {
    while(i > 0)
    {
      if(ipms < gIPMSWin[i - 1])
        gIPMSWin[i] = gIPMSWin[i - 1];
      else
        break;
      i--;
    }
  }
  else // gte, bubble down to repair
  {
    while(i < WINSIZE - 1)
    {
      if(ipms > gIPMSWin[i + 1])
        gIPMSWin[i] = gIPMSWin[i + 1];
      else
        break;
      i++;
    }
  }

  // write it
  gIPMSWin[i] = ipms;

  return gIPMSWin[WINSIZE / 2];
}

static void hook(lua_State *L, lua_Debug *ar);

static void countHook(lua_State *L, lua_Debug *ar, uint64_t cur)
{
  uint64_t elapsed = cur - gLastTime;

  if(elapsed < 1)
    elapsed = 1;

  // instructions / ms
  int ipms = (gLastCount * 1000) / elapsed;
  int medIpms = updateIPMSWin(ipms);

  if(medIpms < 1)
    medIpms = 1;

  gLastCount = medIpms;
  // execute approximately 1ms worth of instructions!
  lua_sethook(L, hook, HOOKMASK, medIpms);

  struct Line *line = getARLine(L, ar, 0);

  line->count++;
  gLastTime = now();
}

static void callHook(lua_State *L, lua_Debug *ar)
{
  uint64_t cur = now();

  if(cur - gLastTime > 10000) // blew past 10ms? pretend count expired
    countHook(L, ar, cur);
}

static void hook(lua_State *L, lua_Debug *ar)
{
  switch(ar->event)
  {
#if CALLCHECK
  case LUA_HOOKCALL:
    callHook(L, ar);
    break;
#endif
  case LUA_HOOKCOUNT:
    countHook(L, ar, now());
    break;
  }
}

LUALIB_API int countprof_start (lua_State *L) {
  lua_sethook(L, hook, HOOKMASK, gLastCount);
  gLastTime = now();
  return 0;
}

LUALIB_API int countprof_stop (lua_State *L) {
  lua_sethook(L, hook, 0, -1);
  return 0;
}

struct Stack
{
  struct Stack *next;
  const char *source;
  struct Line *line;
};

// dump a string for a given stack (with no trailing nl)
static void dumpStack(FILE *f, struct Stack *link)
{
  if(link->next)
  {
    dumpStack(f, link->next);
    fputc(';', f);
  }
  fprintf(f, "%s:%d", link->source, link->line->line);
}

// recursively dump a linked list of sources
static void dumpSources(FILE *f, struct Source *source, struct Stack *next)
{
  struct Stack link = { next };

  while(source)
  {
    struct Line *line = source->lineHead;

    link.source = source->source;

    while(line)
    {
      link.line = line;
#if STACK
      if(line->callHead)
        dumpSources(f, line->callHead, &link);
#endif
      dumpStack(f, &link);
      fprintf(f, " %llu\n", (unsigned long long)line->count);
      line = line->next;
    }
    source = source->next;
  }
}

LUALIB_API int countprof_dump (lua_State *L) {
  char path[256];

  sprintf(path, "%d.cp", (int)getpid());

  FILE *f = fopen(path, "w");

  dumpSources(f, gSourceHead, NULL);
  fclose(f);
  return 0;
}

static const luaL_Reg countproflib[] = {
  {"start", countprof_start },
  {"stop", countprof_stop },
  {"dump", countprof_dump },
  {NULL, NULL}
};

LUALIB_API int luaopen_countprof (lua_State *L) {
  luaL_newlib(L, countproflib);

  return 1;
}
