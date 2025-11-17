/*-------------------------------------------------------------------------
 *
 * pgfile.h
 *	  Simple page-based file writer/reader interface
 *
 * This module provides a simple interface for reading and writing fixed-size
 * pages to files in the PostgreSQL data directory. It's designed for basic
 * page-based storage needs.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/pgfile.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGFILE_H
#define PGFILE_H

#include "storage/fd.h"

/*
 * Page size for our simple page-based files.
 * This is fixed at compile time for simplicity.
 */
#define PGFILE_PAGE_SIZE	8192

/*
 * PgFile - opaque handle for a page-based file
 */
typedef struct PgFile PgFile;

/*
 * Function prototypes
 */

/* Open or create a page-based file in the data directory */
extern PgFile *pgfile_open(const char *filename);

/* Close a page-based file */
extern void pgfile_close(PgFile *file);

/* Write a page at the specified page number (0-indexed) */
extern void pgfile_write_page(PgFile *file, uint32 pageno, const char *buffer);

/* Read a page at the specified page number (0-indexed) */
extern void pgfile_read_page(PgFile *file, uint32 pageno, char *buffer);

/* Sync the file to disk */
extern void pgfile_sync(PgFile *file);

/* Get the total number of pages in the file */
extern uint32 pgfile_num_pages(PgFile *file);

/* Truncate the file to a specific number of pages */
extern void pgfile_truncate(PgFile *file, uint32 num_pages);

/* Delete a page-based file */
extern void pgfile_unlink(const char *filename);

#endif							/* PGFILE_H */
