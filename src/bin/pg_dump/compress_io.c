/*-------------------------------------------------------------------------
 *
 * compress_io.c
 *   Routines for archivers to write an uncompressed or compressed data
 *   stream.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *  The interface for writing to an archive consists of three functions:
 *  AllocateCompressor, WriteDataToArchive and EndCompressor. First you call
 *  AllocateCompressor, then write all the data by calling WriteDataToArchive
 *  as many times as needed, and finally EndCompressor. WriteDataToArchive
 *  and EndCompressor will call the WriteFunc that was provided to
 *  AllocateCompressor for each chunk of compressed data.
 *
 *  The interface for reading an archive consists of just one function:
 *  ReadDataFromArchive. ReadDataFromArchive reads the whole compressed input
 *  stream, by repeatedly calling the given ReadFunc. ReadFunc returns the
 *  compressed data chunk at a time, and ReadDataFromArchive decompresses it
 *  and passes the decompressed data to ahwrite(), until ReadFunc returns 0
 *  to signal EOF.
 *
 *  The interface is the same for compressed and uncompressed streams.
 *
 *
 * IDENTIFICATION
 *     src/bin/pg_dump/compress_io.c
 *
 *-------------------------------------------------------------------------
 */

#include "compress_io.h"

static const char *modulename = gettext_noop("compress_io");

static void ParseCompressionOption(int compression, CompressionAlgorithm *alg,
								   int *level);

/* Routines that support zlib compressed data I/O */
#ifdef HAVE_LIBZ
static void InitCompressorZlib(CompressorState *cs, int level);
static void DeflateCompressorZlib(ArchiveHandle *AH, CompressorState *cs,
								  bool flush);
static void ReadDataFromArchiveZlib(ArchiveHandle *AH, ReadFunc readF);
static size_t WriteDataToArchiveZlib(ArchiveHandle *AH, CompressorState *cs,
									 const char *data, size_t dLen);
static void EndCompressorZlib(ArchiveHandle *AH, CompressorState *cs);

#endif

/* Routines that support uncompressed data I/O */
static void ReadDataFromArchiveNone(ArchiveHandle *AH, ReadFunc readF);
static size_t WriteDataToArchiveNone(ArchiveHandle *AH, CompressorState *cs,
									 const char *data, size_t dLen);

/*
 * Interprets a numeric 'compression' value. The algorithm implied by the
 * value (zlib or none at the moment), is returned in *alg, and the
 * zlib compression level in *level.
 */
static void
ParseCompressionOption(int compression, CompressionAlgorithm *alg, int *level)
{
	if (compression == Z_DEFAULT_COMPRESSION ||
		(compression > 0 && compression <= 9))
		*alg = COMPR_ALG_LIBZ;
	else if (compression == 0)
		*alg = COMPR_ALG_NONE;
	else
		die_horribly(NULL, modulename, "Invalid compression code: %d\n",
					 compression);

	/* The level is just the passed-in value. */
	if (level)
		*level = compression;
}

/* Public interface routines */

/* Allocate a new compressor */
CompressorState *
AllocateCompressor(int compression, WriteFunc writeF)
{
	CompressorState *cs;
	CompressionAlgorithm alg;
	int level;

	ParseCompressionOption(compression, &alg, &level);

#ifndef HAVE_LIBZ
	if (alg == COMPR_ALG_LIBZ)
		die_horribly(NULL, modulename, "not built with zlib support\n");
#endif

	cs = (CompressorState *) calloc(1, sizeof(CompressorState));
	if (cs == NULL)
		die_horribly(NULL, modulename, "out of memory\n");
	cs->writeF = writeF;
	cs->comprAlg = alg;

	/*
	 * Perform compression algorithm specific initialization.
	 */
#ifdef HAVE_LIBZ
	if (alg == COMPR_ALG_LIBZ)
		InitCompressorZlib(cs, level);
#endif

	return cs;
}

/*
 * Read all compressed data from the input stream (via readF) and print it
 * out with ahwrite().
 */
void
ReadDataFromArchive(ArchiveHandle *AH, int compression, ReadFunc readF)
{
	CompressionAlgorithm alg;

	ParseCompressionOption(compression, &alg, NULL);

	if (alg == COMPR_ALG_NONE)
		ReadDataFromArchiveNone(AH, readF);
	if (alg == COMPR_ALG_LIBZ)
	{
#ifdef HAVE_LIBZ
		ReadDataFromArchiveZlib(AH, readF);
#else
		die_horribly(NULL, modulename, "not built with zlib support\n");
#endif
	}
}

/*
 * Compress and write data to the output stream (via writeF).
 */
size_t
WriteDataToArchive(ArchiveHandle *AH, CompressorState *cs,
				   const void *data, size_t dLen)
{
	switch(cs->comprAlg)
	{
		case COMPR_ALG_LIBZ:
#ifdef HAVE_LIBZ
			return WriteDataToArchiveZlib(AH, cs, data, dLen);
#else
			die_horribly(NULL, modulename, "not built with zlib support\n");	
#endif
		case COMPR_ALG_NONE:
			return WriteDataToArchiveNone(AH, cs, data, dLen);
	}
	return 0; /* keep compiler quiet */
}

/*
 * Terminate compression library context and flush its buffers.
 */
void
EndCompressor(ArchiveHandle *AH, CompressorState *cs)
{
#ifdef HAVE_LIBZ
	if (cs->comprAlg == COMPR_ALG_LIBZ)
		EndCompressorZlib(AH, cs);
#endif
	free(cs);
}

/* Private routines, specific to each compression method. */

#ifdef HAVE_LIBZ
/*
 * Functions for zlib compressed output.
 */

static void
InitCompressorZlib(CompressorState *cs, int level)
{
	z_streamp			zp;

	zp = cs->zp = (z_streamp) malloc(sizeof(z_stream));
	if (cs->zp == NULL)
		die_horribly(NULL, modulename, "out of memory\n");
	zp->zalloc = Z_NULL;
	zp->zfree = Z_NULL;
	zp->opaque = Z_NULL;

	/*
	 * zlibOutSize is the buffer size we tell zlib it can output
	 * to.  We actually allocate one extra byte because some routines
	 * want to append a trailing zero byte to the zlib output.
	 */
	cs->zlibOut = (char *) malloc(ZLIB_OUT_SIZE + 1);
	cs->zlibOutSize = ZLIB_OUT_SIZE;

	if (cs->zlibOut == NULL)
		die_horribly(NULL, modulename, "out of memory\n");

	if (deflateInit(zp, level) != Z_OK)
		die_horribly(NULL, modulename,
					 "could not initialize compression library: %s\n",
					 zp->msg);

	/* Just be paranoid - maybe End is called after Start, with no Write */
	zp->next_out = (void *) cs->zlibOut;
	zp->avail_out = cs->zlibOutSize;
}

static void
EndCompressorZlib(ArchiveHandle *AH, CompressorState *cs)
{
	z_streamp			zp = cs->zp;

	zp->next_in = NULL;
	zp->avail_in = 0;

	/* Flush any remaining data from zlib buffer */
	DeflateCompressorZlib(AH, cs, true);

	if (deflateEnd(zp) != Z_OK)
		die_horribly(AH, modulename,
					 "could not close compression stream: %s\n", zp->msg);

	free(cs->zlibOut);
	free(cs->zp);
}

static void
DeflateCompressorZlib(ArchiveHandle *AH, CompressorState *cs, bool flush)
{
	z_streamp	zp = cs->zp;
	char	   *out = cs->zlibOut;
	int			res = Z_OK;

	while (cs->zp->avail_in != 0 || flush)
	{
		res = deflate(zp, flush ? Z_FINISH : Z_NO_FLUSH);
		if (res == Z_STREAM_ERROR)
			die_horribly(AH, modulename,
						 "could not compress data: %s\n", zp->msg);
		if ((flush && (zp->avail_out < cs->zlibOutSize))
			|| (zp->avail_out == 0)
			|| (zp->avail_in != 0)
			)
		{
			/*
			 * Extra paranoia: avoid zero-length chunks, since a zero length
			 * chunk is the EOF marker in the custom format. This should never
			 * happen but...
			 */
			if (zp->avail_out < cs->zlibOutSize)
			{
				/*
				 * Any write function shoud do its own error checking but
				 * to make sure we do a check here as well...
				 */
				size_t len = cs->zlibOutSize - zp->avail_out;
				if (cs->writeF(AH, out, len) != len)
					die_horribly(AH, modulename,
								 "could not write to output file: %s\n",
								 strerror(errno));
			}
			zp->next_out = (void *) out;
			zp->avail_out = cs->zlibOutSize;
		}

		if (res == Z_STREAM_END)
			break;
	}
}

static size_t
WriteDataToArchiveZlib(ArchiveHandle *AH, CompressorState *cs,
					   const char *data, size_t dLen)
{
	cs->zp->next_in = (void *) data;
	cs->zp->avail_in = dLen;
	DeflateCompressorZlib(AH, cs, false);
	/* we have either succeeded in writing dLen bytes or we have called
	 * die_horribly() */
	return dLen;
}

static void
ReadDataFromArchiveZlib(ArchiveHandle *AH, ReadFunc readF)
{
	z_streamp	zp;
	char	   *out;
	int			res = Z_OK;
	size_t		cnt;
	char	   *buf;
	size_t		buflen;

	zp = (z_streamp) malloc(sizeof(z_stream));
	if (zp == NULL)
		die_horribly(NULL, modulename, "out of memory\n");
	zp->zalloc = Z_NULL;
	zp->zfree = Z_NULL;
	zp->opaque = Z_NULL;

	buf = malloc(ZLIB_IN_SIZE);
	if (buf == NULL)
		die_horribly(NULL, modulename, "out of memory\n");
	buflen = ZLIB_IN_SIZE;

	out = malloc(ZLIB_OUT_SIZE + 1);
	if (out == NULL)
		die_horribly(NULL, modulename, "out of memory\n");

	if (inflateInit(zp) != Z_OK)
		die_horribly(NULL, modulename,
					 "could not initialize compression library: %s\n",
					 zp->msg);

	/* no minimal chunk size for zlib */
	while ((cnt = readF(AH, &buf, &buflen)))
	{
		zp->next_in = (void *) buf;
		zp->avail_in = cnt;

		while (zp->avail_in > 0)
		{
			zp->next_out = (void *) out;
			zp->avail_out = ZLIB_OUT_SIZE;

			res = inflate(zp, 0);
			if (res != Z_OK && res != Z_STREAM_END)
				die_horribly(AH, modulename,
							 "could not uncompress data: %s\n", zp->msg);

			out[ZLIB_OUT_SIZE - zp->avail_out] = '\0';
			ahwrite(out, 1, ZLIB_OUT_SIZE - zp->avail_out, AH);
		}
	}

	zp->next_in = NULL;
	zp->avail_in = 0;
	while (res != Z_STREAM_END)
	{
		zp->next_out = (void *) out;
		zp->avail_out = ZLIB_OUT_SIZE;
		res = inflate(zp, 0);
		if (res != Z_OK && res != Z_STREAM_END)
			die_horribly(AH, modulename,
						 "could not uncompress data: %s\n", zp->msg);

		out[ZLIB_OUT_SIZE - zp->avail_out] = '\0';
		ahwrite(out, 1, ZLIB_OUT_SIZE - zp->avail_out, AH);
	}

	if (inflateEnd(zp) != Z_OK)
		die_horribly(AH, modulename,
					 "could not close compression library: %s\n", zp->msg);

	free(buf);
	free(out);
	free(zp);
}

#endif  /* HAVE_LIBZ */


/*
 * Functions for uncompressed output.
 */

static void
ReadDataFromArchiveNone(ArchiveHandle *AH, ReadFunc readF)
{
	size_t		cnt;
	char	   *buf;
	size_t		buflen;

	buf = malloc(ZLIB_OUT_SIZE);
	if (buf == NULL)
		die_horribly(NULL, modulename, "out of memory\n");
	buflen = ZLIB_OUT_SIZE;

	while ((cnt = readF(AH, &buf, &buflen)))
	{
		ahwrite(buf, 1, cnt, AH);
	}

	free(buf);
}

static size_t
WriteDataToArchiveNone(ArchiveHandle *AH, CompressorState *cs,
					   const char *data, size_t dLen)
{
	/*
	 * Any write function should do its own error checking but to make
	 * sure we do a check here as well...
	 */
	if (cs->writeF(AH, data, dLen) != dLen)
		die_horribly(AH, modulename,
					 "could not write to output file: %s\n",
					 strerror(errno));
	return dLen;
}


