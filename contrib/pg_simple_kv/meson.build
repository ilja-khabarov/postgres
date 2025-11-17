# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_simple_kv_sources = files(
  'pg_simple_kv.c',
)

if host_system == 'windows'
  pg_simple_kv_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_simple_kv',
    '--FILEDESC', 'pg_simple_kv - simple persistent key-value store using page-based file I/O',])
endif

# Note: This extension depends on pgfile module from the backend
pg_simple_kv = shared_module('pg_simple_kv',
  pg_simple_kv_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_simple_kv

install_data(
  'pg_simple_kv--1.0.sql',
  'pg_simple_kv.control',
  kwargs: contrib_data_args,
)
