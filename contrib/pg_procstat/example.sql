-- Example usage of pg_procstat extension
-- Demonstrates bidirectional page reading

-- Create the extension
CREATE EXTENSION IF NOT EXISTS pg_procstat;

-- Clear any existing data
SELECT procstat_clear();

-- Record a snapshot of current backends (Snapshot 1)
SELECT procstat_record() AS backends_recorded;

-- Wait 2 seconds
SELECT pg_sleep(2);

-- Record another snapshot (Snapshot 2)
SELECT procstat_record() AS backends_recorded;

-- Wait 2 seconds
SELECT pg_sleep(2);

-- Record another snapshot (Snapshot 3)
SELECT procstat_record() AS backends_recorded;

-- View file statistics
SELECT * FROM procstat_stats();

-------------------------------------------------------------------
-- FORWARD READING (Chronological Order: oldest to newest)
-------------------------------------------------------------------

\echo '--- Reading FORWARD (page 0 → end) ---'

SELECT
    page_number,
    to_char(page_timestamp, 'HH24:MI:SS.MS') as page_time,
    pid,
    state,
    application_name,
    to_char(snapshot_time, 'HH24:MI:SS.MS') as snapshot_time
FROM procstat_read_forward()
ORDER BY page_number, pid;

-------------------------------------------------------------------
-- BACKWARD READING (Reverse Chronological: newest to oldest)
-------------------------------------------------------------------

\echo '--- Reading BACKWARD (end → page 0) ---'

SELECT
    page_number,
    to_char(page_timestamp, 'HH24:MI:SS.MS') as page_time,
    pid,
    state,
    application_name,
    to_char(snapshot_time, 'HH24:MI:SS.MS') as snapshot_time
FROM procstat_read_backward()
ORDER BY page_number DESC, pid;

-------------------------------------------------------------------
-- Demonstrate use cases
-------------------------------------------------------------------

-- Get the most recent snapshot (using backward reading)
\echo '--- Most Recent Snapshot (using backward reading) ---'

WITH first_page AS (
    SELECT DISTINCT page_number
    FROM procstat_read_backward()
    LIMIT 1
)
SELECT
    pid,
    state,
    application_name,
    to_char(backend_start, 'HH24:MI:SS') as backend_start
FROM procstat_read_backward()
WHERE page_number = (SELECT page_number FROM first_page);

-- Get the oldest snapshot (using forward reading)
\echo '--- Oldest Snapshot (using forward reading) ---'

WITH first_page AS (
    SELECT DISTINCT page_number
    FROM procstat_read_forward()
    LIMIT 1
)
SELECT
    pid,
    state,
    application_name,
    to_char(backend_start, 'HH24:MI:SS') as backend_start
FROM procstat_read_forward()
WHERE page_number = (SELECT page_number FROM first_page);

-- Count backends per snapshot
\echo '--- Backend Count Per Snapshot ---'

SELECT
    page_number,
    to_char(page_timestamp, 'HH24:MI:SS.MS') as recorded_at,
    count(*) as backend_count
FROM procstat_read_forward()
GROUP BY page_number, page_timestamp
ORDER BY page_number;

-- Find all snapshots where a specific backend was active
-- (Replace 12345 with an actual PID from your results)
\echo '--- Track specific backend across snapshots ---'

SELECT
    page_number,
    to_char(page_timestamp, 'HH24:MI:SS.MS') as snapshot_at,
    pid,
    state,
    to_char(query_start, 'HH24:MI:SS.MS') as query_start
FROM procstat_read_forward()
WHERE pid = pg_backend_pid()  -- Track current backend
ORDER BY page_number;

-------------------------------------------------------------------
-- Performance comparison (not measurable with small data, but demonstrates pattern)
-------------------------------------------------------------------

\echo '--- Forward vs Backward Reading Pattern ---'

\echo 'Forward reading demonstrates:'
\echo '  - Chronological access (oldest first)'
\echo '  - Useful for historical analysis'
\echo '  - Starts at page 0, increments'

\echo ''
\echo 'Backward reading demonstrates:'
\echo '  - Reverse chronological access (newest first)'
\echo '  - Useful for recent activity queries'
\echo '  - Starts at last page, decrements'

-- Clean up (commented out by default)
-- SELECT procstat_clear();
