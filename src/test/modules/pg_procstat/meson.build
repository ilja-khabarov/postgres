# Copyright (c) 2025, PostgreSQL Global Development Group

pg_procstat_sources = files(
  'pg_procstat.c',
)

if host_system == 'windows'
  pg_procstat_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_procstat',
    '--FILEDESC', 'pg_procstat - test code for procstat file operations',])
endif

pg_procstat = shared_module('pg_procstat',
  pg_procstat_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += pg_procstat

test_install_data += files(
  'pg_procstat.control',
  'pg_procstat--1.0.sql',
)
