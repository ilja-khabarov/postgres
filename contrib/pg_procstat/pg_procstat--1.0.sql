/* contrib/pg_procstat/pg_procstat--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_procstat" to load this file. \quit

--
-- procstat_record() - Record current backend statistics snapshot
--
-- Returns the number of backends recorded
--
CREATE FUNCTION procstat_record()
RETURNS integer
AS 'MODULE_PATHNAME', 'procstat_record'
LANGUAGE C STRICT;

COMMENT ON FUNCTION procstat_record() IS
'Record a snapshot of current backend process statistics';

--
-- procstat_read_forward() - Read all pages in forward (chronological) order
--
-- Returns SETOF records with page and backend information
--
CREATE FUNCTION procstat_read_forward()
RETURNS TABLE(
    page_number integer,
    page_timestamp timestamptz,
    pid integer,
    database_oid oid,
    user_oid oid,
    backend_start timestamptz,
    query_start timestamptz,
    snapshot_time timestamptz,
    application_name text,
    state text
)
AS 'MODULE_PATHNAME', 'procstat_read_forward'
LANGUAGE C STRICT;

COMMENT ON FUNCTION procstat_read_forward() IS
'Read all recorded statistics in forward (chronological) order - demonstrates forward page iteration';

--
-- procstat_read_backward() - Read all pages in backward (reverse chronological) order
--
-- Returns SETOF records with page and backend information
--
CREATE FUNCTION procstat_read_backward()
RETURNS TABLE(
    page_number integer,
    page_timestamp timestamptz,
    pid integer,
    database_oid oid,
    user_oid oid,
    backend_start timestamptz,
    query_start timestamptz,
    snapshot_time timestamptz,
    application_name text,
    state text
)
AS 'MODULE_PATHNAME', 'procstat_read_backward'
LANGUAGE C STRICT;

COMMENT ON FUNCTION procstat_read_backward() IS
'Read all recorded statistics in backward (reverse chronological) order - demonstrates backward page iteration';

--
-- procstat_clear() - Clear all recorded data
--
CREATE FUNCTION procstat_clear()
RETURNS void
AS 'MODULE_PATHNAME', 'procstat_clear'
LANGUAGE C STRICT;

COMMENT ON FUNCTION procstat_clear() IS
'Clear all recorded process statistics data';

--
-- procstat_stats() - Get file statistics
--
CREATE FUNCTION procstat_stats()
RETURNS TABLE(
    total_pages integer,
    file_size bigint,
    page_size integer
)
AS 'MODULE_PATHNAME', 'procstat_stats'
LANGUAGE C STRICT;

COMMENT ON FUNCTION procstat_stats() IS
'Get statistics about the procstat storage file';
