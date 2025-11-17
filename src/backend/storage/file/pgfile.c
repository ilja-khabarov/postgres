/*-------------------------------------------------------------------------
 *
 * pgfile.c
 *	  Simple page-based file writer/reader implementation
 *
 * This module provides a simple interface for reading and writing fixed-size
 * pages to files in the PostgreSQL data directory. Pages are PGFILE_PAGE_SIZE
 * bytes each and are accessed by page number.
 *
 * The implementation uses PostgreSQL's virtual file descriptor (VFD) system
 * to manage file descriptors efficiently.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/pgfile.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "common/file_perm.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/pgfile.h"
#include "utils/memutils.h"

/*
 * PgFile - structure representing an open page-based file
 */
struct PgFile
{
	File		vfd;			/* virtual file descriptor */
	char	   *filename;		/* filename (palloc'd) */
	uint32		num_pages;		/* cached number of pages in file */
};

/*
 * pgfile_open
 *
 * Open or create a page-based file in the PostgreSQL data directory.
 * The file is created if it doesn't exist.
 *
 * Returns a PgFile handle that must be closed with pgfile_close().
 */
PgFile *
pgfile_open(const char *filename)
{
	PgFile	   *pgfile;
	File		vfd;
	char		path[MAXPGPATH];
	struct stat st;

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
	if (FileStat(vfd, &st) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));

	pgfile->num_pages = st.st_size / PGFILE_PAGE_SIZE;

	return pgfile;
}

/*
 * pgfile_close
 *
 * Close a page-based file and free associated resources.
 */
void
pgfile_close(PgFile *file)
{
	if (file == NULL)
		return;

	FileClose(file->vfd);
	pfree(file->filename);
	pfree(file);
}

/*
 * pgfile_write_page
 *
 * Write a page at the specified page number.
 * The buffer must be exactly PGFILE_PAGE_SIZE bytes.
 * If the page number is beyond the current end of file, the file is extended.
 */
void
pgfile_write_page(PgFile *file, uint32 pageno, const char *buffer)
{
	off_t		offset;
	int			nbytes;

	Assert(file != NULL);
	Assert(buffer != NULL);

	/* Calculate the byte offset for this page */
	offset = (off_t) pageno * PGFILE_PAGE_SIZE;

	/* Write the page */
	nbytes = FilePWrite(file->vfd, buffer, PGFILE_PAGE_SIZE, offset,
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
 * pgfile_read_page
 *
 * Read a page at the specified page number.
 * The buffer must be at least PGFILE_PAGE_SIZE bytes.
 * Returns an error if the page number is beyond the end of the file.
 */
void
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
	nbytes = FilePRead(file->vfd, buffer, PGFILE_PAGE_SIZE, offset,
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
 * pgfile_sync
 *
 * Sync the file to disk to ensure durability.
 */
void
pgfile_sync(PgFile *file)
{
	Assert(file != NULL);

	if (FileSync(file->vfd, WAIT_EVENT_DATA_FILE_SYNC) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not sync file \"%s\": %m", file->filename)));
}

/*
 * pgfile_num_pages
 *
 * Get the total number of pages in the file.
 */
uint32
pgfile_num_pages(PgFile *file)
{
	Assert(file != NULL);
	return file->num_pages;
}

/*
 * pgfile_truncate
 *
 * Truncate the file to a specific number of pages.
 * If num_pages is greater than the current size, this is a no-op.
 */
void
pgfile_truncate(PgFile *file, uint32 num_pages)
{
	off_t		new_size;

	Assert(file != NULL);

	/* Calculate the new file size in bytes */
	new_size = (off_t) num_pages * PGFILE_PAGE_SIZE;

	/* Truncate the file */
	if (FileTruncate(file->vfd, new_size, WAIT_EVENT_DATA_FILE_TRUNCATE) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\" to %u pages: %m",
						file->filename, num_pages)));

	/* Update cached page count */
	file->num_pages = num_pages;
}

/*
 * pgfile_unlink
 *
 * Delete a page-based file from the data directory.
 * The file must be closed before calling this function.
 */
void
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
