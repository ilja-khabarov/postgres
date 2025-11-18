# src/test/modules/pg_procstat/Makefile

MODULE_big = pg_procstat
OBJS = \
	$(WIN32RES) \
	pg_procstat.o
PGFILEDESC = "pg_procstat - test code for procstat file operations"

EXTENSION = pg_procstat
DATA = pg_procstat--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/pg_procstat
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
