/*-------------------------------------------------------------------------
 *
 * pg_procstat.c
 *	  Process statistics recorder with bidirectional page-based reading
 *
 * This extension demonstrates advanced page-based file I/O patterns including:
 *   - Custom page headers with metadata
 *   - Bidirectional page reading (forward and backward)
 *   - Efficient page-based storage (integrated pgfile implementation)
 *   - Recording process statistics snapshots over time
 *
 * The extension records PostgreSQL backend process statistics (PID, database,
 * user, query start time, etc.) and stores them in fixed-size pages. Pages
 * can be read in both directions - forward (chronological) or backward
 * (reverse chronological).
 *
 * This extension includes an integrated page-based file I/O implementation
 * (based on pgfile) that provides simple fixed-size page storage.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_procstat/pg_procstat.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <time.h>
#include <unistd.h>

#include "access/xact.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

/* Storage file name */
#define PROCSTAT_FILENAME	"procstat.dat"

/* Page size - fixed at 8KB */
#define PGFILE_PAGE_SIZE	8192
#define PROCSTAT_PAGE_SIZE	PGFILE_PAGE_SIZE

/*
 * =========================================================================
 * FILE I/O HELPERS
 * =========================================================================
 */

/* Helper: Get number of pages in file */
static inline uint32
get_num_pages(File file)
{
	off_t		size = FileSize(file);

	if (size < 0)
		return 0;
	return size / PGFILE_PAGE_SIZE;
}

/*
 * =========================================================================
 * PROCESS STATISTICS RECORDING (PROCSTAT)
 * =========================================================================
 */

/*
 * The page header - page metadata
 */
typedef struct ProcstatPageHeader
{
	Size		size;			/* Size of actual data in this page */
	uint32		record_count;	/* Number of records in this page */
	TimestampTz timestamp;		/* When this page was written */
	uint32		page_number;	/* Sequential page number (0-based) */
} ProcstatPageHeader;

/*
 * The page structure with flexible array member
 */
typedef struct ProcstatPage
{
	ProcstatPageHeader header;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} ProcstatPage;

/* Type alias for clarity */
typedef ProcstatPage *Page;

/*
 * Process statistics record stored in pages
 */
typedef struct ProcstatRecord
{
	int			pid;			/* Process ID */
	Oid			database_oid;	/* Database OID */
	Oid			user_oid;		/* User OID */
	TimestampTz backend_start;	/* Backend start time */
	TimestampTz xact_start;		/* Transaction start time */
	TimestampTz query_start;	/* Query start time */
	TimestampTz state_change;	/* Last state change */
	TimestampTz snapshot_time;	/* When this snapshot was taken */
	char		application_name[64];	/* Application name */
	char		state[16];		/* Current state */
} ProcstatRecord;

/*
 * The file reading context
 */
typedef struct ProcstatFileReader
{
	File		fd;				/* File descriptor */
	int			current_page_idx;	/* Current page index */
	bool		forward;		/* Direction: true=forward, false=backward */
	char		page_buffer[PROCSTAT_PAGE_SIZE]; /* Buffer for current page */
} ProcstatFileReader;

/* Forward declarations */
static void write_page(Page page);
static ProcstatFileReader init_reader(bool forward);
static Page get_next_page(ProcstatFileReader *reader);

/*
 * write_page - Append the page to existing file
 */
static void
write_page(Page page)
{
	File		file;
	char		path[MAXPGPATH];
	uint32		page_num;
	off_t		offset;
	char		page_buffer[PROCSTAT_PAGE_SIZE];

	/* Open file */
	snprintf(path, MAXPGPATH, "%s/%s", DataDir, PROCSTAT_FILENAME);
	file = PathNameOpenFile(path, O_RDWR | O_CREAT | PG_BINARY);
	if (file < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	/* Get next page number */
	page_num = get_num_pages(file);

	/* Update page header */
	page->header.page_number = page_num;
	page->header.timestamp = GetCurrentTimestamp();

	/* Prepare page buffer */
	memset(page_buffer, 0, PROCSTAT_PAGE_SIZE);
	memcpy(page_buffer, &page->header, sizeof(ProcstatPageHeader));
	memcpy(page_buffer + sizeof(ProcstatPageHeader), page->data, page->header.size);

	/* Write page */
	offset = (off_t) page_num * PGFILE_PAGE_SIZE;
	if (FileWrite(file, page_buffer, PGFILE_PAGE_SIZE, offset,
				  WAIT_EVENT_DATA_FILE_WRITE) != PGFILE_PAGE_SIZE)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));

	/* Sync and close */
	FileSync(file, WAIT_EVENT_DATA_FILE_SYNC);
	FileClose(file);
}

/*
 * init_reader - Initialize reader for bidirectional reading
 */
static ProcstatFileReader
init_reader(bool forward)
{
	ProcstatFileReader reader;
	File		file;
	char		path[MAXPGPATH];

	memset(&reader, 0, sizeof(ProcstatFileReader));

	/* Open file */
	snprintf(path, MAXPGPATH, "%s/%s", DataDir, PROCSTAT_FILENAME);
	file = PathNameOpenFile(path, O_RDONLY | PG_BINARY);
	if (file < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	reader.fd = file;
	reader.forward = forward;

	if (forward)
		reader.current_page_idx = 0;
	else
		reader.current_page_idx = get_num_pages(file) - 1;

	return reader;
}

/*
 * get_next_page - Read next page based on direction
 */
static Page
get_next_page(ProcstatFileReader *reader)
{
	int			total_pages;
	off_t		offset;

	/* Get total pages */
	total_pages = get_num_pages(reader->fd);

	/* Check bounds */
	if (reader->forward && reader->current_page_idx >= total_pages)
		return NULL;
	if (!reader->forward && reader->current_page_idx < 0)
		return NULL;

	/* Read the page */
	offset = (off_t) reader->current_page_idx * PGFILE_PAGE_SIZE;
	if (FileRead(reader->fd, reader->page_buffer, PGFILE_PAGE_SIZE, offset,
				 WAIT_EVENT_DATA_FILE_READ) != PGFILE_PAGE_SIZE)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file: %m")));

	/* Advance index */
	if (reader->forward)
		reader->current_page_idx++;
	else
		reader->current_page_idx--;

	return (Page) reader->page_buffer;
}

/*
 * procstat_record - Record current backend statistics
 *
 * Creates a snapshot of current backend process statistics and writes
 * them to a page. Multiple records may fit in one page.
 */
PG_FUNCTION_INFO_V1(procstat_record);
Datum
procstat_record(PG_FUNCTION_ARGS)
{
	ProcstatPage *page;
	ProcstatRecord *records;
	int			num_backends;
	int			i;
	Size		total_size;
	Size		max_records_per_page;

	/* Calculate how many records fit in one page */
	max_records_per_page = (PROCSTAT_PAGE_SIZE - sizeof(ProcstatPageHeader)) / sizeof(ProcstatRecord);

	/* Get backend count */
	num_backends = pgstat_fetch_stat_numbackends();

	if (num_backends == 0)
		PG_RETURN_INT32(0);

	/* Limit to what fits in one page */
	if (num_backends > max_records_per_page)
		num_backends = max_records_per_page;

	/* Allocate page with space for records */
	total_size = sizeof(ProcstatPageHeader) + (num_backends * sizeof(ProcstatRecord));
	page = (ProcstatPage *) palloc0(total_size);
	records = (ProcstatRecord *) page->data;

	/* Collect backend statistics */
	for (i = 0; i < num_backends; i++)
	{
		LocalPgBackendStatus *local_beentry;
		PgBackendStatus *beentry;

		local_beentry = pgstat_get_local_beentry_by_index(i + 1);
		if (!local_beentry)
			continue;

		beentry = &local_beentry->backendStatus;

		records[i].pid = beentry->st_procpid;
		records[i].database_oid = beentry->st_databaseid;
		records[i].user_oid = beentry->st_userid;
		records[i].backend_start = beentry->st_proc_start_timestamp;
		records[i].xact_start = beentry->st_xact_start_timestamp;
		records[i].query_start = beentry->st_activity_start_timestamp;
		records[i].state_change = beentry->st_state_start_timestamp;
		records[i].snapshot_time = GetCurrentTimestamp();

		/* Copy application name */
		if (beentry->st_appname[0])
			strlcpy(records[i].application_name, beentry->st_appname,
					sizeof(records[i].application_name));
		else
			strcpy(records[i].application_name, "");

		/* Copy state */
		switch (beentry->st_state)
		{
			case STATE_IDLE:
				strcpy(records[i].state, "idle");
				break;
			case STATE_RUNNING:
				strcpy(records[i].state, "active");
				break;
			case STATE_IDLEINTRANSACTION:
				strcpy(records[i].state, "idle in xact");
				break;
			case STATE_FASTPATH:
				strcpy(records[i].state, "fastpath");
				break;
			case STATE_IDLEINTRANSACTION_ABORTED:
				strcpy(records[i].state, "idle in xact (aborted)");
				break;
			case STATE_DISABLED:
				strcpy(records[i].state, "disabled");
				break;
			default:
				strcpy(records[i].state, "undefined");
				break;
		}
	}

	/* Set page header */
	page->header.size = num_backends * sizeof(ProcstatRecord);
	page->header.record_count = num_backends;

	/* Write the page */
	write_page(page);

	pfree(page);

	PG_RETURN_INT32(num_backends);
}

/*
 * procstat_read_forward - Read all pages in forward direction
 *
 * Returns SETOF records with page and record information.
 */
PG_FUNCTION_INFO_V1(procstat_read_forward);
Datum
procstat_read_forward(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	ProcstatFileReader *reader;
	Page		page;
	ProcstatRecord *records;
	int		   *current_record_idx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Initialize reader for forward reading */
		reader = (ProcstatFileReader *) palloc(sizeof(ProcstatFileReader));
		*reader = init_reader(true);	/* forward = true */

		/* Initialize current record index */
		current_record_idx = (int *) palloc(sizeof(int));
		*current_record_idx = 0;

		/* Build tuple descriptor */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context that cannot accept type record")));

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx = reader;
		funcctx->max_calls = (uint64) current_record_idx;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	reader = (ProcstatFileReader *) funcctx->user_fctx;
	current_record_idx = (int *) funcctx->max_calls;

	/* Try to get current page or next page */
	while (true)
	{
		if (*current_record_idx == 0)
		{
			/* Need to read next page */
			page = get_next_page(reader);
			if (page == NULL)
			{
				/* No more pages */
				FileClose(reader->fd);
				SRF_RETURN_DONE(funcctx);
			}
			records = (ProcstatRecord *) page->data;
		}
		else
		{
			/* Use current page */
			page = (Page) reader->page_buffer;
			records = (ProcstatRecord *) page->data;
		}

		/* Check if we have more records in current page */
		if (*current_record_idx < page->header.record_count)
		{
			Datum		values[10];
			bool		nulls[10];
			HeapTuple	tuple;
			ProcstatRecord *rec = &records[*current_record_idx];

			/* Prepare tuple */
			memset(nulls, 0, sizeof(nulls));

			values[0] = Int32GetDatum(page->header.page_number);
			values[1] = TimestampTzGetDatum(page->header.timestamp);
			values[2] = Int32GetDatum(rec->pid);
			values[3] = ObjectIdGetDatum(rec->database_oid);
			values[4] = ObjectIdGetDatum(rec->user_oid);
			values[5] = TimestampTzGetDatum(rec->backend_start);
			values[6] = TimestampTzGetDatum(rec->query_start);
			values[7] = TimestampTzGetDatum(rec->snapshot_time);
			values[8] = CStringGetTextDatum(rec->application_name);
			values[9] = CStringGetTextDatum(rec->state);

			tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

			(*current_record_idx)++;
			SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
		}
		else
		{
			/* Move to next page */
			*current_record_idx = 0;
		}
	}
}

/*
 * procstat_read_backward - Read all pages in backward direction
 *
 * Returns SETOF records with page and record information (reverse order).
 */
PG_FUNCTION_INFO_V1(procstat_read_backward);
Datum
procstat_read_backward(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	ProcstatFileReader *reader;
	Page		page;
	ProcstatRecord *records;
	int		   *current_record_idx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Initialize reader for backward reading */
		reader = (ProcstatFileReader *) palloc(sizeof(ProcstatFileReader));
		*reader = init_reader(false);	/* forward = false */

		/* Initialize current record index (-1 means need new page) */
		current_record_idx = (int *) palloc(sizeof(int));
		*current_record_idx = -1;

		/* Build tuple descriptor */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context that cannot accept type record")));

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx = reader;
		funcctx->max_calls = (uint64) current_record_idx;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	reader = (ProcstatFileReader *) funcctx->user_fctx;
	current_record_idx = (int *) funcctx->max_calls;

	/* Try to get current page or next page */
	while (true)
	{
		if (*current_record_idx < 0)
		{
			/* Need to read next page */
			page = get_next_page(reader);
			if (page == NULL)
			{
				/* No more pages */
				FileClose(reader->fd);
				SRF_RETURN_DONE(funcctx);
			}
			records = (ProcstatRecord *) page->data;
			/* Start from last record in page */
			*current_record_idx = page->header.record_count - 1;
		}
		else
		{
			/* Use current page */
			page = (Page) reader->page_buffer;
			records = (ProcstatRecord *) page->data;
		}

		/* Check if we have more records in current page */
		if (*current_record_idx >= 0)
		{
			Datum		values[10];
			bool		nulls[10];
			HeapTuple	tuple;
			ProcstatRecord *rec = &records[*current_record_idx];

			/* Prepare tuple */
			memset(nulls, 0, sizeof(nulls));

			values[0] = Int32GetDatum(page->header.page_number);
			values[1] = TimestampTzGetDatum(page->header.timestamp);
			values[2] = Int32GetDatum(rec->pid);
			values[3] = ObjectIdGetDatum(rec->database_oid);
			values[4] = ObjectIdGetDatum(rec->user_oid);
			values[5] = TimestampTzGetDatum(rec->backend_start);
			values[6] = TimestampTzGetDatum(rec->query_start);
			values[7] = TimestampTzGetDatum(rec->snapshot_time);
			values[8] = CStringGetTextDatum(rec->application_name);
			values[9] = CStringGetTextDatum(rec->state);

			tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

			(*current_record_idx)--;
			SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
		}
		else
		{
			/* Move to next page (backward) */
			*current_record_idx = -1;
		}
	}
}

/*
 * procstat_clear - Clear all recorded data
 */
PG_FUNCTION_INFO_V1(procstat_clear);
Datum
procstat_clear(PG_FUNCTION_ARGS)
{
	char		path[MAXPGPATH];

	snprintf(path, MAXPGPATH, "%s/%s", DataDir, PROCSTAT_FILENAME);
	if (unlink(path) != 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", path)));

	PG_RETURN_VOID();
}

/*
 * procstat_stats - Get file statistics
 */
PG_FUNCTION_INFO_V1(procstat_stats);
Datum
procstat_stats(PG_FUNCTION_ARGS)
{
	File		file;
	char		path[MAXPGPATH];
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3];
	HeapTuple	tuple;
	uint32		num_pages;
	int64		file_size;

	/* Open the file */
	snprintf(path, MAXPGPATH, "%s/%s", DataDir, PROCSTAT_FILENAME);
	file = PathNameOpenFile(path, O_RDONLY | PG_BINARY);
	if (file < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	num_pages = get_num_pages(file);
	file_size = (int64) num_pages * PROCSTAT_PAGE_SIZE;
	FileClose(file);

	/* Build tuple descriptor */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context that cannot accept type record")));

	/* Fill in values */
	memset(nulls, 0, sizeof(nulls));
	values[0] = Int32GetDatum(num_pages);
	values[1] = Int64GetDatum(file_size);
	values[2] = Int32GetDatum(PROCSTAT_PAGE_SIZE);

	tuple = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
