# contrib/pg_simple_kv/Makefile

MODULE_big = pg_simple_kv
OBJS = \
	$(WIN32RES) \
	pg_simple_kv.o

EXTENSION = pg_simple_kv
DATA = pg_simple_kv--1.0.sql
PGFILEDESC = "pg_simple_kv - simple persistent key-value store using page-based file I/O"

# The extension depends on pgfile.o from the backend
# We need to add it to the shared library
SHLIB_LINK += $(top_builddir)/src/backend/storage/file/pgfile.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_simple_kv
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
