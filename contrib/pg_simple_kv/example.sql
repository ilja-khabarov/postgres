-- Example usage of pg_simple_kv extension

-- Create the extension
CREATE EXTENSION IF NOT EXISTS pg_simple_kv;

-- Store some configuration values
SELECT kv_put('db.max_connections', '100');
SELECT kv_put('db.shared_buffers', '128MB');
SELECT kv_put('app.name', 'MyApplication');
SELECT kv_put('app.version', '1.2.3');
SELECT kv_put('feature.analytics', 'enabled');

-- Retrieve specific values
SELECT kv_get('app.name') AS application_name;
SELECT kv_get('db.max_connections') AS max_connections;

-- Check if keys exist
SELECT kv_exists('app.name') AS name_exists;
SELECT kv_exists('nonexistent') AS nonexistent_exists;

-- List all keys
SELECT key FROM kv_list() AS key ORDER BY key;

-- View statistics
SELECT * FROM kv_stats();

-- Update a value
SELECT kv_put('app.version', '1.2.4') AS is_new;  -- Returns false (updated)

-- Delete a value
SELECT kv_delete('feature.analytics') AS deleted;

-- Verify deletion
SELECT kv_get('feature.analytics') AS should_be_null;

-- List remaining keys
SELECT key FROM kv_list() AS key ORDER BY key;

-- Store binary-safe data (works with any text)
SELECT kv_put('json.config', '{"enabled": true, "timeout": 30}');
SELECT kv_get('json.config') AS json_data;

-- Clear everything
-- SELECT kv_clear();

-- Verify empty
-- SELECT * FROM kv_stats();
