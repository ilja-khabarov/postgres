# contrib/pg_procstat/Makefile

MODULE_big = pg_procstat
OBJS = \
	$(WIN32RES) \
	pg_procstat.o

EXTENSION = pg_procstat
DATA = pg_procstat--1.0.sql
PGFILEDESC = "pg_procstat - process statistics recorder with bidirectional page-based reading"

# The extension depends on pgfile.o from the backend
SHLIB_LINK += $(top_builddir)/src/backend/storage/file/pgfile.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_procstat
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
