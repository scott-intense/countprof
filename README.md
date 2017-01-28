# countprof

# build

clang countprof.c -shared -fPIC -o countprof.so

# use

local prof = require'countprof'

prof.start()

-- ...

prof.stop()

prof.dump() -- writes to {pid}.cp
