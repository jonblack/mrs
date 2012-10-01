#include "M6Lib.h"

#define LIBARCHIVE_STATIC
#include <archive.h>
#include <archive_entry.h>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filter/bzip2.hpp>
#include <boost/iostreams/filter/gzip.hpp>

#include "M6DataSource.h"
#include "M6Error.h"
#include "M6Progress.h"
#include "M6File.h"
#include "M6Exec.h"

#if defined(_MSC_VER)
#pragma comment(lib, "libarchive")
#pragma comment(lib, "libbz2")
#endif

using namespace std;
namespace fs = boost::filesystem;
namespace io = boost::iostreams;

// --------------------------------------------------------------------

struct M6DataSourceImpl
{
					M6DataSourceImpl(M6Progress& inProgress) : mProgress(inProgress) {}
	virtual			~M6DataSourceImpl() {}
	
	virtual M6DataSource::M6DataFile*
					Next() = 0;
	
	static M6DataSourceImpl*
					Create(const fs::path& inFile, M6Progress& inProgress);

	M6Progress&		mProgress;
};

// --------------------------------------------------------------------

M6DataSource::iterator::iterator(M6DataSourceImpl* inSource)
	: mSource(inSource), mDataFile(inSource->Next())
{
	if (mDataFile == nullptr)
		mSource = nullptr;
}

M6DataSource::iterator::iterator(const iterator& iter)
	: mSource(iter.mSource), mDataFile(iter.mDataFile)
{
	if (mDataFile != nullptr)
		++mDataFile->mRefCount;
}

M6DataSource::iterator&
M6DataSource::iterator::operator=(const iterator& iter)
{
	if (this != &iter)
	{
		if (mDataFile != nullptr and --mDataFile->mRefCount == 0)
			delete mDataFile;
		mSource = iter.mSource;
		mDataFile = iter.mDataFile;
		if (mDataFile != nullptr)
			++mDataFile->mRefCount;
	}
	return *this;
}

M6DataSource::iterator::~iterator()
{
	delete mDataFile;
}

M6DataSource::iterator& M6DataSource::iterator::operator++()
{
	assert(mSource);
	if (mDataFile != nullptr and --mDataFile->mRefCount == 0)
		delete mDataFile;
	mDataFile = mSource->Next();
	if (mDataFile == nullptr)
		mSource = nullptr;
	return *this;
}

// --------------------------------------------------------------------

struct M6PlainTextDataSourceImpl : public M6DataSourceImpl
{
					M6PlainTextDataSourceImpl(const fs::path& inFile, M6Progress& inProgress)
						: M6DataSourceImpl(inProgress), mFile(inFile) {}
	
	struct device : public io::source
	{
		typedef char			char_type;
		typedef io::source_tag	category;
	
						device(const fs::path inFile, M6Progress& inProgress)
							: mFile(inFile, eReadOnly), mProgress(inProgress) {}

		streamsize		read(char* s, streamsize n)
						{
							if (n > mFile.Size() - mFile.Tell())
								n = mFile.Size() - mFile.Tell();
							if (n > 0)
							{
								mFile.Read(s, n);
								mProgress.Consumed(n);
							}
							else
								n = -1;
							return n;
						}
	
		M6File			mFile;
		M6Progress&		mProgress;
	};

	virtual M6DataSource::M6DataFile*	Next()
					{
						M6DataSource::M6DataFile* result = nullptr;
						if (not mFile.empty())
						{
							result = new M6DataSource::M6DataFile;
							result->mFilename = mFile.filename().string();

							if (mFile.extension() == ".gz")
								result->mStream.push(io::gzip_decompressor());
							else if (mFile.extension() == ".bz2")
								result->mStream.push(io::bzip2_decompressor());
							
							result->mStream.push(device(mFile, mProgress));
							
							mProgress.Message(mFile.filename().string());
							mFile.clear();
						}
						return result;
					}

	fs::path		mFile;
};

// --------------------------------------------------------------------

struct M6ArchiveDataSourceImpl : public M6DataSourceImpl
{
	typedef M6DataSource::M6DataFile M6DataFile;
	
						M6ArchiveDataSourceImpl(struct archive* inArchive, M6Progress& inProgress)
							: M6DataSourceImpl(inProgress), mArchive(inArchive) {}
						~M6ArchiveDataSourceImpl();
	
	struct device : public io::source
	{
		typedef char			char_type;
		typedef io::source_tag	category;
	
						device(struct archive* inArchive, struct archive_entry* inEntry,
								M6Progress& inProgress)
							: mArchive(inArchive), mEntry(inEntry), mProgress(inProgress)
						{
						}

		streamsize		read(char* s, streamsize n)
						{
							streamsize result = -1;
							if (mEntry != nullptr)
							{
								result = archive_read_data(mArchive, s, n);
								if (result == 0)
									mEntry = nullptr;
							}
							
							if (result >= 0)
								mProgress.Progress(archive_position_compressed(mArchive));

							return result;
						}
	
		struct archive*	mArchive;
		struct archive_entry*
						mEntry;
		M6Progress&		mProgress;
	};

	virtual M6DataFile*	Next()
					{
						M6DataFile* result = nullptr;

						if (mArchive != nullptr)
						{
							archive_entry* entry;
							int err = archive_read_next_header(mArchive, &entry);
							if (err == ARCHIVE_OK)
							{
								result = new M6DataFile;
								const char* path = archive_entry_pathname(entry);
								result->mFilename = path;
								result->mStream.push(device(mArchive, entry, mProgress));
								mProgress.Message(path);
							}
							else if (err != ARCHIVE_OK)
								THROW((archive_error_string(mArchive)));
						}
						
						return result;
					}

	struct archive*	mArchive;
};

M6ArchiveDataSourceImpl::~M6ArchiveDataSourceImpl()
{
	if (mArchive != nullptr)
		archive_read_finish(mArchive);
}

// --------------------------------------------------------------------

struct M6FilterDataSourceImpl : public M6DataSourceImpl
{
					M6FilterDataSourceImpl(const fs::path& inFile, M6Progress& inProgress)
						: M6DataSourceImpl(inProgress), mFile(inFile) {}

	static bool		CanFilter(const fs::path& inFile)	{ return M6FilePathNameMatches(inFile.filename(), "*.ags.gz"); }
		
	struct filter_impl : public M6Process
	{
	    typedef char char_type;
					filter_impl(const string& inFilterCommand)
						: M6Process(inFilterCommand) {}
	};
	
	struct filter : boost::iostreams::symmetric_filter<filter_impl>
	{
					filter(const string& inFilterCommand,
						int buffer_size = boost::iostreams::default_device_buffer_size)
						: symmetric_filter(buffer_size, inFilterCommand) {}
	};
	
	struct device : public io::source
	{
		typedef char			char_type;
		typedef io::source_tag	category;
	
						device(const fs::path inFile, M6Progress& inProgress)
							: mFile(inFile, eReadOnly), mProgress(inProgress) {}

		streamsize		read(char* s, streamsize n)
						{
							if (n > mFile.Size() - mFile.Tell())
								n = mFile.Size() - mFile.Tell();
							if (n > 0)
							{
								mFile.Read(s, n);
								mProgress.Consumed(n);
							}
							else
								n = -1;
							return n;
						}
	
		M6File			mFile;
		M6Progress&		mProgress;
	};

	virtual M6DataSource::M6DataFile*	Next()
					{
						M6DataSource::M6DataFile* result = nullptr;
						if (not mFile.empty())
						{
							result = new M6DataSource::M6DataFile;
							result->mFilename = mFile.filename().string();
							
							result->mStream.push(filter("gene2xml -bT"));

							if (mFile.extension() == ".gz")
								result->mStream.push(io::gzip_decompressor());
							else if (mFile.extension() == ".bz2")
								result->mStream.push(io::bzip2_decompressor());
							
							result->mStream.push(device(mFile, mProgress));
							
							mProgress.Message(mFile.filename().string());
							mFile.clear();
						}
						return result;
					}

	fs::path		mFile;
};

// --------------------------------------------------------------------

M6DataSourceImpl* M6DataSourceImpl::Create(const fs::path& inFile, M6Progress& inProgress)
{
	M6DataSourceImpl* result = nullptr;

	if (M6FilterDataSourceImpl::CanFilter(inFile))
		result = new M6FilterDataSourceImpl(inFile, inProgress);
	else
	{
		const uint32 kBufferSize = 4096;
		
		struct archive* archive = archive_read_new();
		
		if (archive == nullptr)
			THROW(("Failed to initialize libarchive"));
		
		int err = archive_read_support_compression_all(archive);
	
		if (err == ARCHIVE_OK)
			err = archive_read_support_format_all(archive);
		
		if (err == ARCHIVE_OK)
			err = archive_read_open_filename(archive, inFile.string().c_str(), kBufferSize);
		
		if (err == ARCHIVE_OK)	// a supported archive
			result = new M6ArchiveDataSourceImpl(archive, inProgress);
		else
		{
			archive_read_finish(archive);
			result = new M6PlainTextDataSourceImpl(inFile, inProgress);
		}
	}
	
	return result;
}

// --------------------------------------------------------------------

M6DataSource::M6DataSource(const fs::path& inFile, M6Progress& inProgress)
	: mImpl(M6DataSourceImpl::Create(inFile, inProgress))
{
}

M6DataSource::~M6DataSource()
{
	delete mImpl;
}
