# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_procstat_sources = files(
  'pg_procstat.c',
)

if host_system == 'windows'
  pg_procstat_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_procstat',
    '--FILEDESC', 'pg_procstat - process statistics recorder with bidirectional page-based reading',])
endif

# Note: This extension depends on pgfile module from the backend
pg_procstat = shared_module('pg_procstat',
  pg_procstat_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_procstat

install_data(
  'pg_procstat--1.0.sql',
  'pg_procstat.control',
  kwargs: contrib_data_args,
)
