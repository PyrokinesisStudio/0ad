#include "precompiled.h"

#include "lib/timer.h"
#include "lib/allocators.h"
#include "lib/res/res.h"
#include "file_internal.h"


// components:
// - za_*: Zip archive handling
//   passes the list of files in an archive to lookup.
// - lookup_*: file lookup
//   per archive: return file info (e.g. offset, size), given filename.
// - Archive_*: Handle-based container for archive info
//   owns archive file and its lookup mechanism.
// - inf_*: in-memory inflate routines (zlib wrapper)
//   decompresses blocks from file_io callback.
// - afile_*: file from Zip archive
//   uses lookup to get file information; holds inflate state.
// - sync and async I/O
//   uses file_* and inf_*.
// - file mapping

///////////////////////////////////////////////////////////////////////////////
//
// lookup_*: file lookup
// per archive: return file info (e.g. offset, size), given filename.
//
///////////////////////////////////////////////////////////////////////////////


// rationale:
// - we don't export a "key" (currently array index) that would allow faster
//   file lookup. this would only be useful if higher-level code were to
//   store the key and use it more than once. also, lookup is currently fast
//   enough. finally, this would also make our file enumerate callback
//   incompatible with the others (due to the extra key param).
//
// - we don't bother with a directory tree to speed up lookup. the above
//   is fast enough: O(1) if accessed sequentially, otherwise O(log(files)).


///////////////////////////////////////////////////////////////////////////////
//
// Archive_*: Handle-based container for archive info
// owns archive file and its lookup mechanism.
//
///////////////////////////////////////////////////////////////////////////////


struct Archive
{
	File f;

	ArchiveEntry* ents;
	// number of valid entries in above array (see lookup_add_file_cb)
	uint num_files;

	// note: we need to keep track of what resources reload() allocated,
	// so the dtor can free everything correctly.
	uint is_open : 1;
	uint is_loaded : 1;
};

H_TYPE_DEFINE(Archive);


static void Archive_init(Archive*, va_list)
{
}

static void Archive_dtor(Archive* a)
{
	if(a->is_loaded)
	{
		(void)mem_free(a->ents);

		a->is_loaded = 0;
	}
	if(a->is_open)
	{
		(void)file_close(&a->f);
		a->is_open = 0;
	}
}

static LibError Archive_reload(Archive* a, const char* fn, Handle)
{
	// (note: don't warn on failure - this happens when
	// vfs_mount blindly archive_open-s a dir)
	RETURN_ERR(file_open(fn, FILE_CACHE_BLOCK, &a->f));
	a->is_open = 1;

	RETURN_ERR(zip_populate_archive(a, &a->f));
	a->is_loaded = 1;

	return ERR_OK;
}

static LibError Archive_validate(const Archive* a)
{
	RETURN_ERR(file_validate(&a->f));

	if(debug_is_pointer_bogus(a->ents))
		return ERR_1;

	return ERR_OK;
}

static LibError Archive_to_string(const Archive* a, char* buf)
{
	snprintf(buf, H_STRING_LEN, "(%u files)", a->num_files);
	return ERR_OK;
}



// open and return a handle to the archive indicated by <fn>.
// somewhat slow - each file is added to an internal index.
Handle archive_open(const char* fn)
{
	TIMER("archive_open");
	return h_alloc(H_Archive, fn);
}


// close the archive <ha> and set ha to 0
LibError archive_close(Handle& ha)
{
	return h_free(ha, H_Archive);
}




// look up ArchiveEntry, given filename (untrusted!).
static LibError archive_get_file_info(Archive* a, const char* fn, uintptr_t memento, ArchiveEntry*& ent)
{
	if(memento)
	{
		ent = (ArchiveEntry*)memento;
		return ERR_OK;
	}
	else
	{
		const char* atom_fn = file_make_unique_fn_copy(fn);
		for(uint i = 0; i < a->num_files; i++)
			if(a->ents[i].atom_fn == atom_fn)
			{
				ent = &a->ents[i];
				return ERR_OK;
			}
	}

	return ERR_FILE_NOT_FOUND;
}


// successively call <cb> for each valid file in the archive <ha>,
// passing the complete path and <user>.
// if it returns a nonzero value, abort and return that, otherwise 0.
LibError archive_enum(const Handle ha, const FileCB cb, const uintptr_t user)
{
	H_DEREF(ha, Archive, a);

	struct stat s;
	memset(&s, 0, sizeof(s));

	for(uint i = 0; i < a->num_files; i++)
	{
		const ArchiveEntry* ent = &a->ents[i];
		s.st_mode  = S_IFREG;
		s.st_size  = (off_t)ent->ucsize;
		s.st_mtime = ent->mtime;
		const uintptr_t memento = (uintptr_t)ent;
		LibError ret = cb(ent->atom_fn, &s, memento, user);
		if(ret != INFO_CB_CONTINUE)
			return ret;
	}

	return ERR_OK;
}


LibError archive_allocate_entries(Archive* a, size_t num_entries)
{
	debug_assert(num_entries != 0);	// =0 makes no sense but wouldn't be fatal

	debug_assert(a->ents == 0);	// must not have been allocated yet
	a->ents = (ArchiveEntry*)mem_alloc(num_entries * sizeof(ArchiveEntry), 32);
	if(!a->ents)
		WARN_RETURN(ERR_NO_MEM);
	return ERR_OK;
}


// add file <fn> to the lookup data structure.
// called from za_enum_files in order (0 <= idx < num_entries).
// the first call notifies us of # entries, so we can allocate memory.
//
// note: ent is only valid during the callback! must be copied or saved.
LibError archive_add_file(Archive* a, const ArchiveEntry* ent)
{
	a->ents[a->num_files++] = *ent;
	return ERR_OK;
}



///////////////////////////////////////////////////////////////////////////////
//
// afile_*: file from Zip archive
// uses lookup to get file information; holds inflate state.
//
///////////////////////////////////////////////////////////////////////////////

// convenience function, allows implementation change in AFile.
// note that size == ucsize isn't foolproof, and adding a flag to
// ofs or size is ugly and error-prone.
// no error checking - always called from functions that check af.
static inline bool is_compressed(AFile* af)
{
	return af->method != CM_NONE;
}




// get file status (size, mtime). output param is zeroed on error.
LibError afile_stat(Handle ha, const char* fn, struct stat* s)
{
	// zero output param in case we fail below.
	memset(s, 0, sizeof(struct stat));

	H_DEREF(ha, Archive, a);

	ArchiveEntry* ent;
	CHECK_ERR(archive_get_file_info(a, fn, 0, ent));

	s->st_size  = ent->ucsize;
	s->st_mtime = ent->mtime;
	return ERR_OK;
}




LibError afile_validate(const AFile* af)
{
	if(!af)
		return ERR_INVALID_PARAM;
	// note: don't check af->ha - it may be freed at shutdown before
	// its files. TODO: revisit once dependency support is added.
	if(!af->fc.size)
		return ERR_1;
	// note: af->ctx is 0 if file is not compressed.

	return ERR_OK;
}

#define CHECK_AFILE(af) CHECK_ERR(afile_validate(af))


// open file, and fill *af with information about it.
// return < 0 on error (output param zeroed). 
LibError afile_open(const Handle ha, const char* fn, uintptr_t memento, int flags, AFile* af)
{
	// zero output param in case we fail below.
	memset(af, 0, sizeof(*af));

	H_DEREF(ha, Archive, a);

	// this is needed for AFile below. optimization: archive_get_file_info
	// wants the original filename, but by passing the unique copy
	// we avoid work there (its file_make_unique_fn_copy returns immediately)
	const char* atom_fn = file_make_unique_fn_copy(fn);

	ArchiveEntry* ent;
	// don't want AFile to contain a ArchiveEntry struct -
	// its ucsize member must be 'loose' for compatibility with File.
	// => need to copy ArchiveEntry fields into AFile.
	RETURN_ERR(archive_get_file_info(a, atom_fn, memento, ent));

	zip_fixup_lfh(&a->f, ent);

	uintptr_t ctx = 0;
	// slight optimization: do not allocate context if not compressed
	if(ent->method != CM_NONE)
	{
		ctx = comp_alloc(CT_DECOMPRESSION, ent->method);
		if(!ctx)
			return ERR_NO_MEM;
	}

	af->fc.flags   = flags;
	af->fc.size    = ent->ucsize;
	af->fc.atom_fn = atom_fn;
	af->ofs       = ent->ofs;
	af->csize     = ent->csize;
	af->method    = ent->method;
	af->ha        = ha;
	af->ctx       = ctx;
	af->is_mapped = 0;
	CHECK_AFILE(af);
	return ERR_OK;
}


// close file.
LibError afile_close(AFile* af)
{
	CHECK_AFILE(af);
	// other AFile fields don't need to be freed/cleared
	comp_free(af->ctx);
	af->ctx = 0;
	return ERR_OK;
}



///////////////////////////////////////////////////////////////////////////////
//
// sync and async I/O
// uses file_* and inf_*.
//
///////////////////////////////////////////////////////////////////////////////


static const size_t CHUNK_SIZE = 16*KiB;

// begin transferring <size> bytes, starting at <ofs>. get result
// with afile_io_wait; when no longer needed, free via afile_io_discard.
LibError afile_io_issue(AFile* af, off_t user_ofs, size_t max_output_size, void* user_buf, AFileIo* io)
{
	// zero output param in case we fail below.
	memset(io, 0, sizeof(AFileIo));

	CHECK_AFILE(af);
	H_DEREF(af->ha, Archive, a);

	// not compressed; we'll just read directly from the archive file.
	// no need to clamp to EOF - that's done already by the VFS.
	if(!is_compressed(af))
	{
		// io->ctx is 0 (due to memset)
		const off_t ofs = af->ofs+user_ofs;
		return file_io_issue(&a->f, ofs, max_output_size, user_buf, &io->io);
	}


	io->ctx = af->ctx;
	io->max_output_size = max_output_size;
	io->user_buf = user_buf;

	const off_t cofs = af->ofs + af->last_cofs;	// needed to determine csize

	// read up to next chunk (so that the next read is aligned -
	// less work for aio) or up to EOF.
	const ssize_t left_in_chunk = CHUNK_SIZE - (cofs % CHUNK_SIZE);
	const ssize_t left_in_file = af->csize - cofs;
	const size_t csize = MIN(left_in_chunk, left_in_file);

	void* cbuf = mem_alloc(csize, 4*KiB);
	if(!cbuf)
		return ERR_NO_MEM;

	CHECK_ERR(file_io_issue(&a->f, cofs, csize, cbuf, &io->io));

	af->last_cofs += (off_t)csize;
	return ERR_OK;
}


// indicates if the IO referenced by <io> has completed.
// return value: 0 if pending, 1 if complete, < 0 on error.
int afile_io_has_completed(AFileIo* io)
{
	return file_io_has_completed(&io->io);
}


// wait until the transfer <io> completes, and return its buffer.
// output parameters are zeroed on error.
LibError afile_io_wait(AFileIo* io, void*& buf, size_t& size)
{
	buf = 0;
	size = 0;

	void* raw_buf;
	size_t raw_size;
	CHECK_ERR(file_io_wait(&io->io, raw_buf, raw_size));

	// file is compressed and we need to decompress
	if(io->ctx)
	{
		comp_set_output(io->ctx, (void*)io->user_buf, io->max_output_size);
		ssize_t ucbytes_output = comp_feed(io->ctx, raw_buf, raw_size);
		free(raw_buf);
		RETURN_ERR(ucbytes_output);

		buf = io->user_buf;
		size = ucbytes_output;
	}
	else
	{
		buf  = raw_buf;
		size = raw_size;
	}

	return ERR_OK;
}


// finished with transfer <io> - free its buffer (returned by afile_io_wait)
LibError afile_io_discard(AFileIo* io)
{
	return file_io_discard(&io->io);
}


LibError afile_io_validate(const AFileIo* io)
{
	if(debug_is_pointer_bogus(io->user_buf))
		return ERR_1;
	// <ctx> and <max_output_size> have no invariants we could check.
	RETURN_ERR(file_io_validate(&io->io));
	return ERR_OK;
}


///////////////////////////////////////////////////////////////////////////////





class Decompressor
{
public:
	Decompressor(uintptr_t comp_ctx_, size_t ucsize_max, bool use_temp_buf_, FileIOCB cb, uintptr_t cb_ctx)
	{
		comp_ctx = comp_ctx_;

		csize_total = 0;
		ucsize_left = ucsize_max;

		use_temp_buf = use_temp_buf_;

		user_cb = cb;
		user_cb_ctx = cb_ctx;
	}

	LibError feed(const void* cblock, size_t csize, size_t* bytes_processed)
	{
		if(use_temp_buf)
			RETURN_ERR(comp_alloc_output(comp_ctx, csize));

		void* ucblock = comp_get_output(comp_ctx);

		const size_t ucsize = comp_feed(comp_ctx, cblock, csize);
		*bytes_processed = ucsize;
		debug_assert(ucsize <= ucsize_left);
		ucsize_left -= ucsize;

		LibError ret = INFO_CB_CONTINUE;
		if(user_cb)
			ret = user_cb(user_cb_ctx, ucblock, ucsize, bytes_processed);
		if(ucsize_left == 0)
			ret = ERR_OK;
		return ret;
	}

	size_t total_csize_fed() const { return csize_total; }

private:
	uintptr_t comp_ctx;

	size_t csize_total;
	size_t ucsize_left;

	bool use_temp_buf;

	// allow user-specified callbacks: "chain" them, because file_io's
	// callback mechanism is already used to return blocks.
	FileIOCB user_cb;
	uintptr_t user_cb_ctx;
};


static LibError decompressor_feed_cb(uintptr_t cb_ctx,
	const void* cblock, size_t csize, size_t* bytes_processed)
{
	Decompressor* d = (Decompressor*)cb_ctx;
	return d->feed(cblock, csize, bytes_processed);
}


// read from the (possibly compressed) file <af> as if it were a normal file.
// starting at the beginning of the logical (decompressed) file,
// skip <ofs> bytes of data; read the next <size> bytes into <*pbuf>.
//
// if non-NULL, <cb> is called for each block read, passing <ctx>.
// if it returns a negative error code,
// the read is aborted and that value is returned.
// the callback mechanism is useful for user progress notification or
// processing data while waiting for the next I/O to complete
// (quasi-parallel, without the complexity of threads).
//
// return bytes read, or a negative error code.
ssize_t afile_read(AFile* af, off_t ofs, size_t size, FileIOBuf* pbuf, FileIOCB cb, uintptr_t cb_ctx)
{
	CHECK_AFILE(af);
	H_DEREF(af->ha, Archive, a);

	if(!is_compressed(af))
	{
		bool we_allocated = (pbuf != FILE_BUF_TEMP) && (*pbuf == FILE_BUF_ALLOC);
		// no need to set last_cofs - only checked if compressed.
		ssize_t bytes_read = file_io(&a->f, af->ofs+ofs, size, pbuf, cb, cb_ctx);
		RETURN_ERR(bytes_read);
		if(we_allocated)
			(void)file_buf_set_real_fn(*pbuf, af->fc.atom_fn);
		return bytes_read;
	}

	debug_assert(af->ctx != 0);

	RETURN_ERR(file_buf_get(pbuf, size, af->fc.atom_fn, af->fc.flags, cb));
	const bool use_temp_buf = (pbuf == FILE_BUF_TEMP);
	if(!use_temp_buf)
		comp_set_output(af->ctx, (void*)*pbuf, size);

	const off_t cofs = af->ofs+af->last_cofs;
	// remaining bytes in file. callback will cause IOs to stop when
	// enough ucdata has been produced.
	const size_t csize_max = af->csize - af->last_cofs;

	Decompressor d(af->ctx, size, use_temp_buf, cb, cb_ctx);
	ssize_t uc_transferred = file_io(&a->f, cofs, csize_max, FILE_BUF_TEMP, decompressor_feed_cb, (uintptr_t)&d);

	af->last_cofs += (off_t)d.total_csize_fed();

	return uc_transferred;
}


///////////////////////////////////////////////////////////////////////////////
//
// file mapping
//
///////////////////////////////////////////////////////////////////////////////


// map the entire file <af> into memory. mapping compressed files
// isn't allowed, since the compression algorithm is unspecified.
// output parameters are zeroed on failure.
//
// the mapping will be removed (if still open) when its file is closed.
// however, map/unmap calls should still be paired so that the mapping
// may be removed when no longer needed.
LibError afile_map(AFile* af, void*& p, size_t& size)
{
	p = 0;
	size = 0;

	CHECK_AFILE(af);

	// mapping compressed files doesn't make sense because the
	// compression algorithm is unspecified - disallow it.
	if(is_compressed(af))
		WARN_RETURN(ERR_IS_COMPRESSED);

	// note: we mapped the archive in archive_open, but unmapped it
	// in the meantime to save memory in case it wasn't going to be mapped.
	// now we do so again; it's unmapped in afile_unmap (refcounted).
	H_DEREF(af->ha, Archive, a);
	void* archive_p;
	size_t archive_size;
	CHECK_ERR(file_map(&a->f, archive_p, archive_size));

	p = (char*)archive_p + af->ofs;
	size = af->fc.size;

	af->is_mapped = 1;
	return ERR_OK;
}


// remove the mapping of file <af>; fail if not mapped.
//
// the mapping will be removed (if still open) when its archive is closed.
// however, map/unmap calls should be paired so that the archive mapping
// may be removed when no longer needed.
LibError afile_unmap(AFile* af)
{
	CHECK_AFILE(af);

	// make sure archive mapping refcount remains balanced:
	// don't allow multiple|"false" unmaps.
	if(!af->is_mapped)
		return ERR_FAIL;
	af->is_mapped = 0;

	H_DEREF(af->ha, Archive, a);
	return file_unmap(&a->f);
}


//-----------------------------------------------------------------------------
// archive builder
//-----------------------------------------------------------------------------

static inline bool file_type_is_uncompressible(const char* fn)
{
	const char* ext = strrchr(fn, '.');
	// no extension? bail; assume compressible
	if(!ext)
		return true;

	// this is a selection of file types that are certainly not
	// further compressible. we need not include every type under the sun -
	// this is only a slight optimization that avoids wasting time
	// compressing files. the real decision as to cmethod is made based
	// on attained compression ratio.
	static const char* uncompressible_exts[] =
	{
		"zip", "rar",
			"jpg", "jpeg", "png",
			"ogg", "mp3"
	};

	for(uint i = 0; i < ARRAY_SIZE(uncompressible_exts); i++)
	{
		if(!stricmp(ext+1, uncompressible_exts[i]))
			return true;
	}

	return false;
}


struct CompressParams
{
	bool attempt_compress;
	uintptr_t ctx;
	u32 crc;
};

#include <zlib.h>

static LibError compress_cb(uintptr_t cb_ctx, const void* block, size_t size, size_t* bytes_processed)
{
	CompressParams* p = (CompressParams*)cb_ctx;

	// comp_feed already makes note of total #bytes fed, and we need
	// vfs_io to return the uc size (to check if all data was read).
	*bytes_processed = size;

	// update checksum
	p->crc = crc32(p->crc, (const Bytef*)block, (uInt)size);

	if(p->attempt_compress)
		(void)comp_feed(p->ctx, block, size);
	return INFO_CB_CONTINUE;
}


static LibError read_and_compress_file(const char* atom_fn, uintptr_t ctx,
	ArchiveEntry& ent, void*& file_contents, FileIOBuf& buf)	// out
{
	struct stat s;
	RETURN_ERR(vfs_stat(atom_fn, &s));
	const size_t ucsize = s.st_size;

	const bool attempt_compress = !file_type_is_uncompressible(atom_fn);
	if(attempt_compress)
	{
		RETURN_ERR(comp_reset(ctx));
		RETURN_ERR(comp_alloc_output(ctx, ucsize));
	}

	// read file into newly allocated buffer. if attempt_compress, also
	// compress the file into another buffer while waiting for IOs.
	Handle hf = vfs_open(atom_fn, 0);
	RETURN_ERR(hf);
	buf = FILE_BUF_ALLOC;
	CompressParams params = { attempt_compress, ctx, 0 };
	ssize_t ucsize_read = vfs_io(hf, ucsize, &buf, compress_cb, (uintptr_t)&params);
	debug_assert(ucsize_read == (ssize_t)ucsize);
	(void)vfs_close(hf);

	// if we compressed the file trial-wise, check results and
	// decide whether to store as such or not (based on compression ratio)
	bool store_compressed = false;
	void* cdata = 0; size_t csize = 0;
	if(attempt_compress)
	{
		RETURN_ERR(comp_finish(ctx, &cdata, &csize));

		const float ratio = (float)ucsize / csize;
		const ssize_t bytes_saved = (ssize_t)ucsize - (ssize_t)csize;
		if(ratio > 1.05f && bytes_saved > 200)
			store_compressed = true;
	}

	// store file info
	ent.ucsize  = (off_t)ucsize;
	ent.mtime   = s.st_mtime;
	// .. ent.ofs is set by zip_archive_add_file
	ent.flags   = 0;
	ent.atom_fn = atom_fn;
	ent.crc32   = params.crc;
	if(store_compressed)
	{
		ent.method = CM_DEFLATE;
		ent.csize  = (off_t)csize;
		file_contents = cdata;
	}
	else
	{
		ent.method = CM_NONE;
		ent.csize  = (off_t)ucsize;
		file_contents = (void*)buf;
	}

	// note: no need to free cdata - it is owned by the
	// compression context and can be reused.

	return ERR_OK;
}


LibError archive_build(const char* P_archive_filename, Filenames V_fl)
{
	ZipArchive* za;
	RETURN_ERR(zip_archive_create(P_archive_filename, &za));
	uintptr_t ctx = comp_alloc(CT_COMPRESSION, CM_DEFLATE);

	const char* fn;	// declare outside loop for easier debugging
	for(size_t i = 0; ; i++)
	{
		fn = V_fl[i];
		if(!fn)
			break;

		ArchiveEntry ent; void* file_contents; FileIOBuf buf;
		if(read_and_compress_file(fn, ctx, ent, file_contents, buf) == ERR_OK)
		{
			(void)zip_archive_add_file(za, &ent, file_contents);
			(void)file_buf_free(buf);
		}
	}

	// note: this is currently known to fail if there are no files in the list
	// - zlib.h says: Z_DATA_ERROR is returned if freed prematurely.
	// safe to ignore.
	comp_free(ctx);
	(void)zip_archive_finish(za);

	return ERR_OK;
}