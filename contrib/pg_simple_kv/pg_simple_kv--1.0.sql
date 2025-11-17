/* contrib/pg_simple_kv/pg_simple_kv--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_simple_kv" to load this file. \quit

-- Store a key-value pair
-- Returns true if inserted, false if updated
CREATE FUNCTION kv_put(key text, value text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'kv_put'
LANGUAGE C STRICT;

-- Retrieve a value by key
-- Returns NULL if key doesn't exist
CREATE FUNCTION kv_get(key text)
RETURNS text
AS 'MODULE_PATHNAME', 'kv_get'
LANGUAGE C STRICT;

-- Delete a key
-- Returns true if deleted, false if key didn't exist
CREATE FUNCTION kv_delete(key text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'kv_delete'
LANGUAGE C STRICT;

-- Check if a key exists
CREATE FUNCTION kv_exists(key text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'kv_exists'
LANGUAGE C STRICT;

-- Clear all data from the store
CREATE FUNCTION kv_clear()
RETURNS void
AS 'MODULE_PATHNAME', 'kv_clear'
LANGUAGE C STRICT;

-- List all keys in the store
CREATE FUNCTION kv_list()
RETURNS SETOF text
AS 'MODULE_PATHNAME', 'kv_list'
LANGUAGE C STRICT;

-- Get statistics about the store
CREATE FUNCTION kv_stats()
RETURNS TABLE(
    num_entries integer,
    total_pages integer,
    next_free_page integer,
    file_size bigint
)
AS 'MODULE_PATHNAME', 'kv_stats'
LANGUAGE C STRICT;

-- Convenience: kv_set as alias for kv_put
CREATE FUNCTION kv_set(key text, value text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'kv_put'
LANGUAGE C STRICT;
