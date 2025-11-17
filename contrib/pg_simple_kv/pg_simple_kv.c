/*-------------------------------------------------------------------------
 *
 * pg_simple_kv.c
 *	  Simple persistent key-value store using page-based file I/O
 *
 * This extension demonstrates the use of the pgfile module by implementing
 * a simple persistent key-value store. Data is stored in fixed-size pages
 * using the pgfile API.
 *
 * Storage format:
 *   Page 0: Metadata page
 *     - [uint32] magic number (0x534B5601 = "SKV\x01")
 *     - [uint32] version
 *     - [uint32] number of entries
 *     - [uint32] next free page
 *
 *   Page 1+: Key-value record pages
 *     - [uint8]  status (0=deleted, 1=active)
 *     - [uint32] key length
 *     - [uint32] value length
 *     - [bytes]  key data
 *     - [bytes]  value data
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_simple_kv/pg_simple_kv.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <string.h>

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/pgfile.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/* Storage file name */
#define KV_FILENAME		"simple_kv.dat"

/* Magic number for file format validation */
#define KV_MAGIC		0x534B5601	/* "SKV\x01" */
#define KV_VERSION		1

/* Record status */
#define KV_STATUS_DELETED	0
#define KV_STATUS_ACTIVE	1

/* Metadata page structure (page 0) */
typedef struct KVMetadata
{
	uint32		magic;
	uint32		version;
	uint32		num_entries;
	uint32		next_free_page;
	char		padding[PGFILE_PAGE_SIZE - 16];
} KVMetadata;

/* Maximum sizes */
#define KV_MAX_KEY_LEN		(PGFILE_PAGE_SIZE / 4)
#define KV_MAX_VALUE_LEN	(PGFILE_PAGE_SIZE - 9 - KV_MAX_KEY_LEN)

/* Record header */
typedef struct KVRecordHeader
{
	uint8		status;
	uint32		key_len;
	uint32		value_len;
} KVRecordHeader;

/*
 * Initialize the metadata page if the file is empty
 */
static void
kv_init_if_needed(PgFile *file)
{
	KVMetadata	metadata;

	if (pgfile_num_pages(file) == 0)
	{
		/* Initialize metadata */
		memset(&metadata, 0, sizeof(KVMetadata));
		metadata.magic = KV_MAGIC;
		metadata.version = KV_VERSION;
		metadata.num_entries = 0;
		metadata.next_free_page = 1;

		/* Write metadata page */
		pgfile_write_page(file, 0, (char *) &metadata);
		pgfile_sync(file);
	}
	else
	{
		/* Verify existing metadata */
		pgfile_read_page(file, 0, (char *) &metadata);

		if (metadata.magic != KV_MAGIC)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid magic number in key-value store file")));

		if (metadata.version != KV_VERSION)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("unsupported key-value store version: %u", metadata.version)));
	}
}

/*
 * Read metadata from page 0
 */
static void
kv_read_metadata(PgFile *file, KVMetadata *metadata)
{
	pgfile_read_page(file, 0, (char *) metadata);
}

/*
 * Write metadata to page 0
 */
static void
kv_write_metadata(PgFile *file, KVMetadata *metadata)
{
	pgfile_write_page(file, 0, (char *) metadata);
}

/*
 * Find a key in the store. Returns page number if found, 0 if not found.
 */
static uint32
kv_find_key(PgFile *file, const char *key, uint32 key_len, char *page_buf)
{
	KVMetadata	metadata;
	uint32		page;
	KVRecordHeader *header;

	kv_read_metadata(file, &metadata);

	/* Search all pages for the key */
	for (page = 1; page < pgfile_num_pages(file); page++)
	{
		pgfile_read_page(file, page, page_buf);
		header = (KVRecordHeader *) page_buf;

		/* Skip deleted records */
		if (header->status != KV_STATUS_ACTIVE)
			continue;

		/* Check if key matches */
		if (header->key_len == key_len &&
			memcmp(page_buf + sizeof(KVRecordHeader), key, key_len) == 0)
			return page;
	}

	return 0;					/* Not found */
}

/*
 * kv_put(key text, value text) - Store a key-value pair
 */
PG_FUNCTION_INFO_V1(kv_put);
Datum
kv_put(PG_FUNCTION_ARGS)
{
	text	   *key_text = PG_GETARG_TEXT_PP(0);
	text	   *value_text = PG_GETARG_TEXT_PP(1);
	char	   *key = VARDATA_ANY(key_text);
	char	   *value = VARDATA_ANY(value_text);
	uint32		key_len = VARSIZE_ANY_EXHDR(key_text);
	uint32		value_len = VARSIZE_ANY_EXHDR(value_text);
	PgFile	   *file;
	KVMetadata	metadata;
	char		page_buf[PGFILE_PAGE_SIZE];
	KVRecordHeader *header;
	uint32		page;
	bool		is_update = false;

	/* Validate input sizes */
	if (key_len == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("key cannot be empty")));

	if (key_len > KV_MAX_KEY_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("key too long: %u bytes (maximum %u bytes)",
						key_len, KV_MAX_KEY_LEN)));

	if (value_len > KV_MAX_VALUE_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("value too long: %u bytes (maximum %u bytes)",
						value_len, KV_MAX_VALUE_LEN)));

	/* Open the file */
	file = pgfile_open(KV_FILENAME);
	kv_init_if_needed(file);

	/* Check if key already exists */
	page = kv_find_key(file, key, key_len, page_buf);
	if (page > 0)
	{
		/* Update existing record */
		is_update = true;
	}
	else
	{
		/* Get next free page for new record */
		kv_read_metadata(file, &metadata);
		page = metadata.next_free_page;
		metadata.next_free_page++;
		metadata.num_entries++;
		kv_write_metadata(file, &metadata);
	}

	/* Prepare record */
	memset(page_buf, 0, PGFILE_PAGE_SIZE);
	header = (KVRecordHeader *) page_buf;
	header->status = KV_STATUS_ACTIVE;
	header->key_len = key_len;
	header->value_len = value_len;

	/* Copy key and value */
	memcpy(page_buf + sizeof(KVRecordHeader), key, key_len);
	memcpy(page_buf + sizeof(KVRecordHeader) + key_len, value, value_len);

	/* Write the page */
	pgfile_write_page(file, page, page_buf);
	pgfile_sync(file);

	/* Close the file */
	pgfile_close(file);

	PG_RETURN_BOOL(!is_update);
}

/*
 * kv_get(key text) - Retrieve a value by key
 */
PG_FUNCTION_INFO_V1(kv_get);
Datum
kv_get(PG_FUNCTION_ARGS)
{
	text	   *key_text = PG_GETARG_TEXT_PP(0);
	char	   *key = VARDATA_ANY(key_text);
	uint32		key_len = VARSIZE_ANY_EXHDR(key_text);
	PgFile	   *file;
	char		page_buf[PGFILE_PAGE_SIZE];
	KVRecordHeader *header;
	uint32		page;
	text	   *result;

	/* Open the file */
	file = pgfile_open(KV_FILENAME);
	kv_init_if_needed(file);

	/* Find the key */
	page = kv_find_key(file, key, key_len, page_buf);
	if (page == 0)
	{
		pgfile_close(file);
		PG_RETURN_NULL();
	}

	/* Extract the value */
	header = (KVRecordHeader *) page_buf;
	result = (text *) palloc(VARHDRSZ + header->value_len);
	SET_VARSIZE(result, VARHDRSZ + header->value_len);
	memcpy(VARDATA(result),
		   page_buf + sizeof(KVRecordHeader) + header->key_len,
		   header->value_len);

	pgfile_close(file);
	PG_RETURN_TEXT_P(result);
}

/*
 * kv_delete(key text) - Delete a key
 */
PG_FUNCTION_INFO_V1(kv_delete);
Datum
kv_delete(PG_FUNCTION_ARGS)
{
	text	   *key_text = PG_GETARG_TEXT_PP(0);
	char	   *key = VARDATA_ANY(key_text);
	uint32		key_len = VARSIZE_ANY_EXHDR(key_text);
	PgFile	   *file;
	char		page_buf[PGFILE_PAGE_SIZE];
	KVRecordHeader *header;
	KVMetadata	metadata;
	uint32		page;

	/* Open the file */
	file = pgfile_open(KV_FILENAME);
	kv_init_if_needed(file);

	/* Find the key */
	page = kv_find_key(file, key, key_len, page_buf);
	if (page == 0)
	{
		pgfile_close(file);
		PG_RETURN_BOOL(false);
	}

	/* Mark as deleted */
	header = (KVRecordHeader *) page_buf;
	header->status = KV_STATUS_DELETED;
	pgfile_write_page(file, page, page_buf);

	/* Update metadata */
	kv_read_metadata(file, &metadata);
	metadata.num_entries--;
	kv_write_metadata(file, &metadata);

	pgfile_sync(file);
	pgfile_close(file);

	PG_RETURN_BOOL(true);
}

/*
 * kv_exists(key text) - Check if a key exists
 */
PG_FUNCTION_INFO_V1(kv_exists);
Datum
kv_exists(PG_FUNCTION_ARGS)
{
	text	   *key_text = PG_GETARG_TEXT_PP(0);
	char	   *key = VARDATA_ANY(key_text);
	uint32		key_len = VARSIZE_ANY_EXHDR(key_text);
	PgFile	   *file;
	char		page_buf[PGFILE_PAGE_SIZE];
	uint32		page;

	/* Open the file */
	file = pgfile_open(KV_FILENAME);
	kv_init_if_needed(file);

	/* Find the key */
	page = kv_find_key(file, key, key_len, page_buf);

	pgfile_close(file);
	PG_RETURN_BOOL(page > 0);
}

/*
 * kv_clear() - Clear all data
 */
PG_FUNCTION_INFO_V1(kv_clear);
Datum
kv_clear(PG_FUNCTION_ARGS)
{
	PgFile	   *file;
	KVMetadata	metadata;

	/* Open the file */
	file = pgfile_open(KV_FILENAME);

	/* Truncate to just the metadata page */
	pgfile_truncate(file, 1);

	/* Reset metadata */
	memset(&metadata, 0, sizeof(KVMetadata));
	metadata.magic = KV_MAGIC;
	metadata.version = KV_VERSION;
	metadata.num_entries = 0;
	metadata.next_free_page = 1;

	kv_write_metadata(file, &metadata);
	pgfile_sync(file);
	pgfile_close(file);

	PG_RETURN_VOID();
}

/*
 * kv_list() - List all keys (returns SETOF text)
 */
PG_FUNCTION_INFO_V1(kv_list);
Datum
kv_list(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	PgFile	   *file;
	char	   *page_buf;
	uint32	   *current_page;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Open file and initialize */
		file = pgfile_open(KV_FILENAME);
		kv_init_if_needed(file);

		/* Allocate page buffer in multi-call context */
		page_buf = (char *) palloc(PGFILE_PAGE_SIZE);

		/* Store file handle and page buffer in user_fctx */
		funcctx->user_fctx = file;

		/* Store current page number in max_calls (reusing field) */
		current_page = (uint32 *) palloc(sizeof(uint32));
		*current_page = 1;		/* Start from page 1 */
		funcctx->max_calls = (uint64) current_page;

		/* Store page_buf in attinmeta (reusing field) */
		funcctx->attinmeta = (AttInMetadata *) page_buf;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	file = (PgFile *) funcctx->user_fctx;
	current_page = (uint32 *) funcctx->max_calls;
	page_buf = (char *) funcctx->attinmeta;

	/* Iterate through pages */
	while (*current_page < pgfile_num_pages(file))
	{
		KVRecordHeader *header;
		text	   *result;

		pgfile_read_page(file, *current_page, page_buf);
		header = (KVRecordHeader *) page_buf;

		(*current_page)++;

		/* Skip deleted records */
		if (header->status != KV_STATUS_ACTIVE)
			continue;

		/* Extract key */
		result = (text *) palloc(VARHDRSZ + header->key_len);
		SET_VARSIZE(result, VARHDRSZ + header->key_len);
		memcpy(VARDATA(result),
			   page_buf + sizeof(KVRecordHeader),
			   header->key_len);

		SRF_RETURN_NEXT(funcctx, PointerGetDatum(result));
	}

	/* Done - clean up */
	pgfile_close(file);
	SRF_RETURN_DONE(funcctx);
}

/*
 * kv_stats() - Return statistics about the store
 */
PG_FUNCTION_INFO_V1(kv_stats);
Datum
kv_stats(PG_FUNCTION_ARGS)
{
	PgFile	   *file;
	KVMetadata	metadata;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4];
	HeapTuple	tuple;

	/* Open the file */
	file = pgfile_open(KV_FILENAME);
	kv_init_if_needed(file);
	kv_read_metadata(file, &metadata);

	/* Build tuple descriptor */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context that cannot accept type record")));

	/* Fill in values */
	memset(nulls, 0, sizeof(nulls));
	values[0] = Int32GetDatum(metadata.num_entries);
	values[1] = Int32GetDatum(pgfile_num_pages(file));
	values[2] = Int32GetDatum(metadata.next_free_page);
	values[3] = Int64GetDatum((int64) pgfile_num_pages(file) * PGFILE_PAGE_SIZE);

	tuple = heap_form_tuple(tupdesc, values, nulls);

	pgfile_close(file);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
