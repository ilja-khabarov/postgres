/*-------------------------------------------------------------------------
 *
 * pg_procstat.c
 *		Test module for procstat file operations.
 *
 * This module provides test functions for writing and reading integers
 * to/from files in the PostgreSQL data directory.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/pg_procstat/pg_procstat.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

#define PROCSTAT_TEST_FILE "pg_procstat_test.dat"

PG_FUNCTION_INFO_V1(qdb_procstat_test_write);
PG_FUNCTION_INFO_V1(qdb_procstat_test_read);

/*
 * qdb_procstat_test_write
 *		Write an integer value to a test file.
 *
 * This function writes a single integer to a file in the PostgreSQL
 * data directory. The file is created if it doesn't exist, or
 * overwritten if it does.
 */
Datum
qdb_procstat_test_write(PG_FUNCTION_ARGS)
{
	int32		value = PG_GETARG_INT32(0);
	char		filepath[MAXPGPATH];
	int			fd;
	ssize_t		written;

	/* Construct the file path in the data directory */
	snprintf(filepath, MAXPGPATH, "%s/%s", DataDir, PROCSTAT_TEST_FILE);

	/* Open file for writing, create if doesn't exist */
	fd = OpenTransientFile(filepath, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for writing: %m", filepath)));

	/* Write the integer value */
	written = write(fd, &value, sizeof(int32));
	if (written != sizeof(int32))
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", filepath)));
	}

	/* Close the file */
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", filepath)));

	ereport(NOTICE,
			(errmsg("wrote value %d to file \"%s\"", value, filepath)));

	PG_RETURN_VOID();
}

/*
 * qdb_procstat_test_read
 *		Read an integer value from the test file.
 *
 * This function reads a single integer from the test file created by
 * qdb_procstat_test_write. Returns the integer value, or raises an
 * error if the file doesn't exist or can't be read.
 */
Datum
qdb_procstat_test_read(PG_FUNCTION_ARGS)
{
	char		filepath[MAXPGPATH];
	int			fd;
	int32		value;
	ssize_t		bytes_read;

	/* Construct the file path in the data directory */
	snprintf(filepath, MAXPGPATH, "%s/%s", DataDir, PROCSTAT_TEST_FILE);

	/* Open file for reading */
	fd = OpenTransientFile(filepath, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m", filepath)));

	/* Read the integer value */
	bytes_read = read(fd, &value, sizeof(int32));
	if (bytes_read != sizeof(int32))
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		if (bytes_read < 0)
		{
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from file \"%s\": %m", filepath)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("file \"%s\" contains incomplete data", filepath)));
		}
	}

	/* Close the file */
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", filepath)));

	ereport(NOTICE,
			(errmsg("read value %d from file \"%s\"", value, filepath)));

	PG_RETURN_INT32(value);
}
