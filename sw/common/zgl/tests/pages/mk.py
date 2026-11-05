# zgl_chunky.c with its register accesses as calls, for the host model
import re, sys
s = open(sys.argv[1]).read()
s = re.sub(r'\*z_gtri_reg\((\w+)\)\s*=(?!=)\s*([^;]+);', r'gtri_wr(\1, \2);', s)
s = re.sub(r'\*z_gtri_reg\((\w+)\)', r'gtri_rd(\1)', s)
s = s.replace('#include "zgl.h"', '#include "../../zgl.h"')
s = s.replace('#include "zgl_int.h"', '#include "../../zgl_int.h"')
s = s.replace('#include "../zsoc.h"', '#include "stubs.h"')
s = s.replace('#include "../zeitlos.h"', '')
s = s.replace('#include "zgl_t2.h"', '#include "../../zgl_t2.h"')
open(sys.argv[2], 'w').write(s)
