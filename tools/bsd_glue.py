#!/usr/bin/env python3
"""Файл-связка драйвера BSD: tools/bsd_glue.py ВЫХОД.c ИМЯ_УСТРОЙСТВА cfattach  ->  kd_ca / kd_devname для userland/bsd_rt.c."""
import sys
out, name, ca = sys.argv[1:4]
open(out, 'w').write('#include <bsd_core.h>\nextern const struct cfattach %s;\nconst struct cfattach* kd_ca = &%s;\nconst char* kd_devname = "%s";\n' % (ca, ca, name))
