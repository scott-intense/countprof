# countprof

sampling profiler hack for lua 5.3

samples at approx. 1KHz

generates framegraph.pl-style output

assumes that lua_Debug->source strings remain valid! (so beware if using loadstring, etc.)

# build

clang countprof.c -shared -fPIC -o countprof.so

# use

local prof = require'countprof'

prof.start()

-- ...

prof.stop()

prof.dump() -- writes to {pid}.cp
