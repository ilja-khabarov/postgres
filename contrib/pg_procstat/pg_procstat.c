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
 * INTEGRATED PAGE-BASED FILE I/O IMPLEMENTATION
 * =========================================================================
 *
 * This section provides a simple page-based file I/O interface integrated
 * directly into the extension. It manages fixed-size pages (8KB) stored
 * in files within the PostgreSQL data directory.
 */

/*
 * PgFile - structure representing an open page-based file
 */
typedef struct PgFile
{
	File		vfd;			/* virtual file descriptor */
	char	   *filename;		/* filename (palloc'd) */
	uint32		num_pages;		/* cached number of pages in file */
} PgFile;

/* Forward declarations for integrated pgfile functions */
static PgFile *pgfile_open(const char *filename);
static void pgfile_close(PgFile *file);
static void pgfile_write_page(PgFile *file, uint32 pageno, const char *buffer);
static void pgfile_read_page(PgFile *file, uint32 pageno, char *buffer);
static void pgfile_sync(PgFile *file);
static uint32 pgfile_num_pages(PgFile *file);
static void pgfile_unlink(const char *filename);

/*
 * pgfile_open - Open or create a page-based file
 *
 * Opens a file in the PostgreSQL data directory. Creates it if it doesn't exist.
 * Returns a PgFile handle that must be closed with pgfile_close().
 */
static PgFile *
pgfile_open(const char *filename)
{
	PgFile	   *pgfile;
	File		vfd;
	char		path[MAXPGPATH];
	off_t		file_size;

	/* Build the full path in the data directory */
	snprintf(path, MAXPGPATH, "%s/%s", DataDir, filename);

	/* Open or create the file */
	vfd = PathNameOpenFile(path, O_RDWR | O_CREAT | PG_BINARY);
	if (vfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	/* Allocate and initialize the PgFile structure */
	pgfile = (PgFile *) MemoryContextAlloc(TopMemoryContext, sizeof(PgFile));
	pgfile->vfd = vfd;
	pgfile->filename = MemoryContextStrdup(TopMemoryContext, filename);

	/* Determine the current number of pages in the file */
	file_size = FileSize(vfd);
	if (file_size < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not get size of file \"%s\": %m", path)));

	pgfile->num_pages = file_size / PGFILE_PAGE_SIZE;

	return pgfile;
}

/*
 * pgfile_close - Close a page-based file
 *
 * Closes the file and frees associated resources.
 */
static void
pgfile_close(PgFile *file)
{
	if (file == NULL)
		return;

	FileClose(file->vfd);
	pfree(file->filename);
	pfree(file);
}

/*
 * pgfile_write_page - Write a page at the specified page number
 *
 * The buffer must be exactly PGFILE_PAGE_SIZE bytes.
 * If the page number is beyond the current end of file, the file is extended.
 */
static void
pgfile_write_page(PgFile *file, uint32 pageno, const char *buffer)
{
	off_t		offset;
	int			nbytes;

	Assert(file != NULL);
	Assert(buffer != NULL);

	/* Calculate the byte offset for this page */
	offset = (off_t) pageno * PGFILE_PAGE_SIZE;

	/* Write the page */
	nbytes = FileWrite(file->vfd, buffer, PGFILE_PAGE_SIZE, offset,
					   WAIT_EVENT_DATA_FILE_WRITE);

	if (nbytes != PGFILE_PAGE_SIZE)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\" at offset %llu: %m",
							file->filename, (unsigned long long) offset)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_IO_ERROR),
					 errmsg("could not write to file \"%s\" at offset %llu: wrote only %d of %d bytes",
							file->filename, (unsigned long long) offset,
							nbytes, PGFILE_PAGE_SIZE)));
	}

	/* Update cached page count if we extended the file */
	if (pageno >= file->num_pages)
		file->num_pages = pageno + 1;
}

/*
 * pgfile_read_page - Read a page at the specified page number
 *
 * The buffer must be at least PGFILE_PAGE_SIZE bytes.
 * Returns an error if the page number is beyond the end of the file.
 */
static void
pgfile_read_page(PgFile *file, uint32 pageno, char *buffer)
{
	off_t		offset;
	int			nbytes;

	Assert(file != NULL);
	Assert(buffer != NULL);

	/* Check if the page number is valid */
	if (pageno >= file->num_pages)
		ereport(ERROR,
				(errcode(ERRCODE_IO_ERROR),
				 errmsg("cannot read page %u from file \"%s\": file has only %u pages",
						pageno, file->filename, file->num_pages)));

	/* Calculate the byte offset for this page */
	offset = (off_t) pageno * PGFILE_PAGE_SIZE;

	/* Read the page */
	nbytes = FileRead(file->vfd, buffer, PGFILE_PAGE_SIZE, offset,
					  WAIT_EVENT_DATA_FILE_READ);

	if (nbytes != PGFILE_PAGE_SIZE)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from file \"%s\" at offset %llu: %m",
							file->filename, (unsigned long long) offset)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_IO_ERROR),
					 errmsg("could not read from file \"%s\" at offset %llu: read only %d of %d bytes",
							file->filename, (unsigned long long) offset,
							nbytes, PGFILE_PAGE_SIZE)));
	}
}

/*
 * pgfile_sync - Sync the file to disk
 *
 * Ensures all written data is durably stored on disk.
 */
static void
pgfile_sync(PgFile *file)
{
	Assert(file != NULL);

	if (FileSync(file->vfd, WAIT_EVENT_DATA_FILE_SYNC) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not sync file \"%s\": %m", file->filename)));
}

/*
 * pgfile_num_pages - Get the total number of pages in the file
 */
static uint32
pgfile_num_pages(PgFile *file)
{
	Assert(file != NULL);
	return file->num_pages;
}

/*
 * pgfile_unlink - Delete a page-based file
 *
 * Deletes the file from the data directory.
 * The file must be closed before calling this function.
 */
static void
pgfile_unlink(const char *filename)
{
	char		path[MAXPGPATH];

	/* Build the full path in the data directory */
	snprintf(path, MAXPGPATH, "%s/%s", DataDir, filename);

	/* Delete the file */
	if (unlink(path) < 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not unlink file \"%s\": %m", path)));
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
	uint32		magic;			/* Magic number for validation */
	uint32		checksum;		/* Simple checksum of data */
} ProcstatPageHeader;

/* Magic number for page validation */
#define PROCSTAT_PAGE_MAGIC		0x50535441	/* "PSTA" */

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
	PgFile	   *file;			/* pgfile handle */
	int			current_page_idx;	/* Current page index */
	int			total_pages;	/* Total pages in file */
	bool		forward;		/* Direction: true=forward, false=backward */
	char		page_buffer[PROCSTAT_PAGE_SIZE]; /* Buffer for current page */
	bool		initialized;	/* Reader initialized? */
	bool		exhausted;		/* All pages read? */
} ProcstatFileReader;

/* Forward declarations */
static void write_page(Page page);
static ProcstatFileReader init_reader(bool forward);
static Page get_next_page(ProcstatFileReader *reader);
static uint32 calculate_checksum(const char *data, Size size);
static void validate_page(Page page);

/*
 * calculate_checksum - Simple checksum for page data
 */
static uint32
calculate_checksum(const char *data, Size size)
{
	uint32		checksum = 0;
	Size		i;

	for (i = 0; i < size; i++)
		checksum = ((checksum << 5) + checksum) + (unsigned char) data[i];

	return checksum;
}

/*
 * validate_page - Validate page header and checksum
 */
static void
validate_page(Page page)
{
	uint32		expected_checksum;

	if (page->header.magic != PROCSTAT_PAGE_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid page magic number: expected 0x%08X, got 0x%08X",
						PROCSTAT_PAGE_MAGIC, page->header.magic)));

	if (page->header.size > PROCSTAT_PAGE_SIZE - sizeof(ProcstatPageHeader))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid page size: %zu", page->header.size)));

	/* Verify checksum */
	expected_checksum = calculate_checksum(page->data, page->header.size);
	if (page->header.checksum != expected_checksum)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("page checksum mismatch: expected 0x%08X, got 0x%08X",
						expected_checksum, page->header.checksum)));
}

/*
 * write_page - Append the page to existing file or create a new one
 */
static void
write_page(Page page)
{
	PgFile	   *file;
	uint32		next_page_num;
	char		page_buffer[PROCSTAT_PAGE_SIZE];

	Assert(page != NULL);
	Assert(page->header.size <= PROCSTAT_PAGE_SIZE - sizeof(ProcstatPageHeader));

	/* Open the file */
	file = pgfile_open(PROCSTAT_FILENAME);

	/* Get the next page number (total pages = next page number) */
	next_page_num = pgfile_num_pages(file);

	/* Update page header with metadata */
	page->header.page_number = next_page_num;
	page->header.timestamp = GetCurrentTimestamp();
	page->header.magic = PROCSTAT_PAGE_MAGIC;
	page->header.checksum = calculate_checksum(page->data, page->header.size);

	/* Prepare full page buffer (header + data) */
	memset(page_buffer, 0, PROCSTAT_PAGE_SIZE);
	memcpy(page_buffer, &page->header, sizeof(ProcstatPageHeader));
	memcpy(page_buffer + sizeof(ProcstatPageHeader), page->data, page->header.size);

	/* Write the page (this appends to the file) */
	pgfile_write_page(file, next_page_num, page_buffer);
	pgfile_sync(file);

	pgfile_close(file);
}

/*
 * init_reader - Initialize reader for bidirectional reading
 *
 * forward=true:  Read from start (page 0) to end
 * forward=false: Read from end to start (reverse)
 */
static ProcstatFileReader
init_reader(bool forward)
{
	ProcstatFileReader reader;
	PgFile	   *file;

	memset(&reader, 0, sizeof(ProcstatFileReader));

	/* Open the file */
	file = pgfile_open(PROCSTAT_FILENAME);

	/* Initialize reader context */
	reader.file = file;
	reader.total_pages = pgfile_num_pages(file);
	reader.forward = forward;
	reader.initialized = true;
	reader.exhausted = (reader.total_pages == 0);

	if (forward)
	{
		/* Forward reading: start at page 0 */
		reader.current_page_idx = 0;
	}
	else
	{
		/* Backward reading: start at last page */
		reader.current_page_idx = reader.total_pages - 1;
	}

	return reader;
}

/*
 * get_next_page - Read next page based on direction
 *
 * Returns pointer to page in reader's buffer, or NULL if no more pages.
 * The returned pointer is valid until the next call to get_next_page().
 */
static Page
get_next_page(ProcstatFileReader *reader)
{
	Page		page;

	Assert(reader != NULL);
	Assert(reader->initialized);

	/* Check if exhausted */
	if (reader->exhausted)
		return NULL;

	/* Check bounds */
	if (reader->forward)
	{
		if (reader->current_page_idx >= reader->total_pages)
		{
			reader->exhausted = true;
			return NULL;
		}
	}
	else
	{
		if (reader->current_page_idx < 0)
		{
			reader->exhausted = true;
			return NULL;
		}
	}

	/* Read the current page */
	pgfile_read_page(reader->file, reader->current_page_idx, reader->page_buffer);

	/* Cast buffer to page structure */
	page = (Page) reader->page_buffer;

	/* Validate page */
	validate_page(page);

	/* Advance to next page based on direction */
	if (reader->forward)
		reader->current_page_idx++;
	else
		reader->current_page_idx--;

	return page;
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
				pgfile_close(reader->file);
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
				pgfile_close(reader->file);
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
	pgfile_unlink(PROCSTAT_FILENAME);
	PG_RETURN_VOID();
}

/*
 * procstat_stats - Get file statistics
 */
PG_FUNCTION_INFO_V1(procstat_stats);
Datum
procstat_stats(PG_FUNCTION_ARGS)
{
	PgFile	   *file;
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3];
	HeapTuple	tuple;
	uint32		num_pages;
	int64		file_size;

	/* Open the file */
	file = pgfile_open(PROCSTAT_FILENAME);
	num_pages = pgfile_num_pages(file);
	file_size = (int64) num_pages * PROCSTAT_PAGE_SIZE;
	pgfile_close(file);

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
