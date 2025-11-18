/* src/test/modules/pg_procstat/pg_procstat--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_procstat" to load this file. \quit

--
-- qdb_procstat_test_write(integer)
--
-- Write an integer value to a test file in the data directory
--
CREATE FUNCTION qdb_procstat_test_write(value pg_catalog.int4)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

--
-- qdb_procstat_test_read()
--
-- Read the integer value from the test file
--
CREATE FUNCTION qdb_procstat_test_read()
RETURNS pg_catalog.int4 STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
