// FTP Mirror code
//
//	TODO: unescape percentage escaped characters in URL

#include "M6Lib.h"

#include <time.h>
#include <iostream>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/regex.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#define foreach BOOST_FOREACH
#include <boost/asio.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/function.hpp>

#include "M6Config.h"
#include "M6Error.h"
#include "M6Progress.h"
#include "M6File.h"
#include "M6Exec.h"

using namespace std;
namespace zx = zeep::xml;
namespace fs = boost::filesystem;
namespace ba = boost::algorithm;
namespace at = boost::asio::ip;

// --------------------------------------------------------------------
// Regular expression to parse directory listings

#define list_filetype		"([-dlpscbD])"		// 1
#define list_permission		"[-rwxtTsS]"
#define list_statusflags	list_filetype list_permission "{9}"
								// mon = 3, day = 4, year or hour = 5, minutes = 6
#define list_date			"(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) +(\\d+) +(\\d+)(?::(\\d+))?"
								// size = 2, name = 7
#define list_line			list_statusflags "\\s+\\d+\\s+\\w+\\s+\\w+\\s+"	\
								"(\\d+)\\s+" list_date " (.+)"

const boost::regex kM6LineParserRE(list_line);

const char* kM6Months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// --------------------------------------------------------------------
// Regular expression to parse URL's

#define url_hexdigit		"[[:digit:]a-fA-F]"
#define url_unreserved		"[-[:alnum:]._~]"
#define url_pct_encoded		"%" url_hexdigit "{2}"
#define url_sub_delims		"[!$&'()*+,;=]"
#define url_userinfo		"(?:((?:" url_unreserved "|" url_pct_encoded "|" url_sub_delims ")+)@)?"
#define url_host			"(\\[(?:[[:digit:]a-fA-F:]+)\\]|(?:" url_unreserved "|" url_pct_encoded "|" url_sub_delims ")+)"
#define url_port			"(?::([[:digit:]]+))?"
#define url_pchar			url_unreserved "|" url_pct_encoded "|" url_sub_delims "|:|@"
#define url_path			"(?:/((?:" url_pchar "|\\*|\\?|/)*))?"
	
const boost::regex kM6URLParserRE("(?:ftp|rsync)://" url_userinfo url_host url_port url_path);

// --------------------------------------------------------------------

struct M6FetcherImpl
{
						M6FetcherImpl(const zx::element* inConfig)
							: mConfig(inConfig) {}
	virtual				~M6FetcherImpl() {}

	virtual void		Mirror(bool inDryRun, ostream& out) = 0;

	const zx::element*	mConfig;
};

// --------------------------------------------------------------------

struct M6FTPFetcherImpl : public M6FetcherImpl
{
						M6FTPFetcherImpl(const zx::element* inConfig);

	virtual void		Mirror(bool inDryRun, ostream& out);

	void				Login();

	int64				CollectFiles();
	int64				CollectFiles(fs::path inLocalDir, fs::path inRemoteDir,
							fs::path::iterator p, fs::path::iterator e, M6Progress& inProgress);
	
	uint32				WaitForReply(); 
	uint32				SendAndWaitForReply(const string& inCommand, const string& inParam); 
	
	void				ListFiles(const string& inPattern,
							boost::function<void(char inType, const string&, size_t, time_t)> inProc);
	void				FetchFile(fs::path inRemote, fs::path inLocal, time_t inTime,
							M6Progress& inProgress);
	
	void				Retrieve();

	void				Error(const char* inError);
	
	fs::path			GetPWD();

	string				mDatabank;
	boost::asio::io_service
						mIOService;
	at::tcp::resolver	mResolver;
	at::tcp::socket		mSocket;

	string				mServer, mUser, mPassword, mPort;
	
	string				mReply;
	string				mSource;
	fs::path			mSrcPath;

	struct FileToFetch
	{
		fs::path		local, remote;
		size_t			size;
		time_t			time;
	};

	vector<FileToFetch>	mFilesToFetch;
	vector<fs::path>	mFilesToDelete;
};

M6FTPFetcherImpl::M6FTPFetcherImpl(const zx::element* inConfig)
	: M6FetcherImpl(inConfig)
	, mResolver(mIOService), mSocket(mIOService)
{
	mDatabank = inConfig->get_attribute("id");
	
	zx::element* source = inConfig->find_first("source");
	if (source == nullptr)
		THROW(("Missing source?"));
	mSource = source->content();

	string src = source->get_attribute("fetch");
	boost::smatch m;
	if (not boost::regex_match(src, m, kM6URLParserRE))
		THROW(("Invalid source url: <%s>", src.c_str()));
	
	mServer = m[2];
	mUser = m[1];
	if (mUser.empty())
		mUser = "anonymous";
	mPort = m[3];
	if (mPort.empty())
		mPort = "ftp";
	mSrcPath = fs::path(m[4].str());
}

void M6FTPFetcherImpl::Mirror(bool inDryRun, ostream& out)
{
	zx::element* source = mConfig->find_first("source");

//    string dst = source->get_attribute("dst");
//    if (dst.empty() and inConfig->find_first("source") != nullptr)
//    {
//		fs::path sd(inConfig->find_first("source")->content());
//		while (not sd.empty() and sd.filename().string().find_first_of("*?") != string::npos)
//			sd = sd.parent_path();
//		if (not sd.empty())
//			dst = sd.string();
//    }
//    
//    if (dst.empty())
//        dst = inConfig->get_attribute("id");
//
//    fs::path rawdir = M6Config::GetDirectory("raw");
//    mDstDir = rawdir / dst;
    
	// Get a list of endpoints corresponding to the server name.
	at::tcp::resolver::query query(mServer, mPort);
	at::tcp::resolver::iterator endpoint_iterator = mResolver.resolve(query);
	at::tcp::resolver::iterator end;

	// Try each endpoint until we successfully establish a connection.
	boost::system::error_code error = boost::asio::error::host_not_found;
    while (error && endpoint_iterator != end)
    {
		mSocket.close();
		mSocket.connect(*endpoint_iterator++, error);
	}

	if (error)
		throw boost::system::system_error(error);

	uint32 status = WaitForReply();
	if (status != 220)
		Error("Failed to connect");

	Login();

	int64 bytesToFetch = CollectFiles();
	
	if (inDryRun)
	{
		foreach (auto file, mFilesToFetch)
			out << "fetching " << file.remote << " => " << file.local << endl;
		
		if (source->get_attribute("delete") == "true")
		{
			foreach (auto del, mFilesToDelete)
				out << "Deleting " << del << endl;
		}
	}
	else
	{
		if (not mFilesToFetch.empty())
		{
			M6Progress progress(mDatabank, bytesToFetch, "Fetching");
			
			foreach (auto file, mFilesToFetch)
			{
				progress.Message(string("Fetching ") + file.remote.filename().string());
	
				if (VERBOSE)
					cerr << "fetching " << file.remote << " => " << file.local << endl;
				
				FetchFile(file.remote, file.local, file.time, progress);
			}
		}
		
		if (source->get_attribute("delete") == "true")
		{
			foreach (auto del, mFilesToDelete)
			{
				if (VERBOSE)
					cerr << "Deleting " << del << endl;
				
				fs::remove(del);
			}
		}
	}
}

void M6FTPFetcherImpl::Error(const char* inError)
{
	THROW(("%s: %s\n(trying to access server %s)",
		inError, mReply.c_str(), mServer.c_str()));
}

void M6FTPFetcherImpl::Login()
{
	uint32 status = SendAndWaitForReply("user", mUser);
	if (status == 331)
		status = SendAndWaitForReply("pass", mPassword);
	if (status != 230 and status != 202)
		Error("Error logging in");
}

fs::path M6FTPFetcherImpl::GetPWD()
{
	uint32 status = SendAndWaitForReply("pwd", "");
	if (status != 257)
		Error("Error getting current working directory");
	
	boost::regex re("257 \"((?:[^\"]|\"\")+)\"( .*)?\\s*$");
	boost::smatch m;
	if (not boost::regex_match(mReply, m, re))
		Error("Invalid reply to PWD command");
	
	string path = m[1];
	ba::replace_all(path, "\"\"", "\"");
	return fs::path(path);
}

uint32 M6FTPFetcherImpl::SendAndWaitForReply(const string& inCommand, const string& inParam)
{
	if (VERBOSE)
		cerr << "---> " << inCommand << ' ' << inParam << endl;
	
	boost::asio::streambuf request;
	ostream request_stream(&request);
	request_stream << inCommand << ' ' << inParam << "\r\n";
	boost::asio::write(mSocket, request);

	return WaitForReply();
}

uint32 M6FTPFetcherImpl::WaitForReply()
{
	boost::asio::streambuf response;
	boost::asio::read_until(mSocket, response, "\r\n");

	istream response_stream(&response);

	string line;
	getline(response_stream, line);
	
	if (VERBOSE)
		cerr << line << endl;

	if (line.length() < 3 or not isdigit(line[0]) or not isdigit(line[1]) or not isdigit(line[2]))
		THROW(("FTP Server returned unexpected line:\n\"%s\"", line.c_str()));

	uint32 result = ((line[0] - '0') * 100) + ((line[1] - '0') * 10) + (line[2] - '0');
	mReply = line;

	if (line.length() >= 4 and line[3] == '-')
	{
		string test(line.substr(0, 3) + ' ');

		do
		{
			boost::asio::read_until(mSocket, response, "\r\n");
			istream response_stream(&response);
			getline(response_stream, line);

			if (VERBOSE)
				cerr << line << endl;

			mReply = mReply + '\n' + line;
		}
		while (not ba::starts_with(line, test));
	}

	return result;
}

int64 M6FTPFetcherImpl::CollectFiles()
{
	int64 result = 0;
	
	foreach (fs::path dir, mSrcPath)
	{
		uint32 status = SendAndWaitForReply("cwd", dir.string());
		if (status != 250)
			Error((string("Error changing directory to ") + dir.string()).c_str());
	}
	
	vector<string> sources;
	ba::split(sources, mSource, ba::is_any_of(";"));
	
	M6Progress progress(mDatabank, "listing files");
	
	foreach (const string& source, sources)
	{
		fs::path p(source);
		
		result += CollectFiles(M6Config::GetDirectory("raw"), GetPWD(), p.begin(), p.end(), progress);
		
		progress.Consumed(1);
	}
	
	return result;
}

int64 M6FTPFetcherImpl::CollectFiles(fs::path inLocalDir, fs::path inRemoteDir, fs::path::iterator p, fs::path::iterator e,
	M6Progress& inProgress)
{
	string s = p->string();
	bool isPattern = ba::contains(s, "*") or ba::contains(s, "?");
	++p;
	int64 result = 0;

	vector<string> dirs;

	if (p == e)
	{
		inProgress.Consumed(1);

			// we've reached the end of the url
		if (not fs::exists(inLocalDir))
			fs::create_directories(inLocalDir);

		// Do a listing, regardless the name is a pattern or not
		vector<fs::path> existing;

		fs::directory_iterator end;
		for (fs::directory_iterator file(inLocalDir); file != end; ++file)
		{
			if (fs::is_regular_file(*file))
				existing.push_back(*file);
		}

		ListFiles(s, [this, &existing, &inLocalDir, &inRemoteDir, &result, &inProgress]
			(char inType, const string& inFile, size_t inSize, time_t inTime)
		{
			fs::path file = inLocalDir / inFile;

			bool fetch = inType == '-';

			if (fs::exists(file))
			{
				existing.erase(find(existing.begin(), existing.end(), file), existing.end());

				if (fs::last_write_time(file) >= inTime and fs::file_size(file) == inSize)
				{
					if (VERBOSE)
						cerr << file << " is up-to-date " << endl;
					fetch = false;
				}
			}

			if (fetch)
			{
				FileToFetch need = { file, inRemoteDir / inFile, inSize, inTime };
				this->mFilesToFetch.push_back(need);
				result += inSize;
			}

			inProgress.Consumed(1);
		});
		
		// but only delete files if the name was a pattern
		if (isPattern)
		{
			foreach (fs::path file, existing)
				mFilesToDelete.push_back(file);
		}
	}
	else if (isPattern)
	{
		ListFiles(s, [&dirs, &s](char inType, const string& inFile, size_t inSize, time_t inTime)
		{
			if (inType == 'd')
				dirs.push_back(inFile);
		});
	}
	else
		dirs.push_back(s);

	foreach (const string& dir, dirs)
	{
		uint32 status = SendAndWaitForReply("cwd", dir);
		if (status != 250)
			Error((string("Error changing directory to ") + dir).c_str());
		
		fs::path remoteDir = GetPWD();
		
		fs::path localDir(inLocalDir);
		if (not inLocalDir.empty())
			localDir /= dir;
		else if (isPattern)
			localDir = dir;
		
		result += CollectFiles(localDir, remoteDir, p, e, inProgress);
		status = SendAndWaitForReply("cdup", "");
		if (status != 200 and status != 250)
			Error("Error changing directory");
	}
	
	return result;
}

void M6FTPFetcherImpl::ListFiles(const string& inPattern,
	boost::function<void(char, const string&, size_t, time_t)> inProc)
{
	uint32 status = SendAndWaitForReply("pasv", "");
	if (status != 227)
		Error("Passive mode failed");
	
	boost::regex re("227 Entering Passive Mode \\((\\d+,\\d+,\\d+,\\d+),(\\d+),(\\d+)\\).*");
	boost::smatch m;
	if (not boost::regex_match(mReply, m, re))
		Error("Invalid reply for passive command");
	
	string address = m[1];
	ba::replace_all(address, ",", ".");
	
	string port = boost::lexical_cast<string>(
		boost::lexical_cast<uint32>(m[2]) * 256 +
		boost::lexical_cast<uint32>(m[3]));

	at::tcp::iostream stream;
//	stream.expires_from_now(boost::posix_time::seconds(60));
	stream.connect(address, port);
		
	// Yeah, we have a data connection, now send the List command
	status = SendAndWaitForReply("list", "");
	
	if (status != 125 and status != 150)
		Error((string("Error listing: ") + mReply).c_str());
		
	time_t now;
	time(&now);
	struct tm n;
#ifdef _MSC_VER
	gmtime_s(&n, &now);
#else
	gmtime_r(&now, &n);
#endif
	
	for (;;)
	{
		string line;
		getline(stream, line);

		if (line.empty() and stream.eof())
			break;
		
		ba::trim(line);
		
		if (boost::regex_match(line, m, kM6LineParserRE))
		{
			struct tm t = {};
			
			t.tm_mday = boost::lexical_cast<int>(m[4]);
			for (t.tm_mon = 0; t.tm_mon < 12; ++t.tm_mon)
			{
				if (ba::iequals(kM6Months[t.tm_mon], m[3].str()))
					break;
			}

			if (m[6].str().empty())
				t.tm_year = boost::lexical_cast<int>(m[5]) - 1900;
			else
			{
				t.tm_year = n.tm_year;
				t.tm_hour = boost::lexical_cast<int>(m[5]);
				t.tm_min = boost::lexical_cast<int>(m[6]);
			}
			
			string file = m[7];
			size_t size = boost::lexical_cast<size_t>(m[2]);
			time_t time = mktime(&t);
			
			if (line[0] == 'l')
			{
				string::size_type i = file.find(" -> ");
				if (i != string::npos)
					file.erase(i, string::npos);
			}
			
			if (inPattern.empty() or M6FilePathNameMatches(file, inPattern))
				inProc(line[0], file, size, time);
		}
	}
	
	status = WaitForReply();
	if (status != 226)
		Error("Error listing");
}

void M6FTPFetcherImpl::FetchFile(fs::path inRemote, fs::path inLocal, time_t inTime, M6Progress& inProgress)
{
	fs::path local(inLocal.branch_path() / (inLocal.filename().string() + ".tmp"));
	
	if (fs::exists(local))
		fs::remove(local);
	
	fs::ofstream file(local, ios::binary);
	if (not file.is_open())
		Error("Could not create local file");

	uint32 status = SendAndWaitForReply("type", "I");
	if (status != 200)
		Error("Setting TYPE to I failed");

	status = SendAndWaitForReply("pasv", "");
	if (status != 227)
		Error("Passive mode failed");
	
	boost::regex re("227 Entering Passive Mode \\((\\d+,\\d+,\\d+,\\d+),(\\d+),(\\d+)\\).*");
	boost::smatch m;
	if (not boost::regex_match(mReply, m, re))
		Error("Invalid reply for passive command");
	
	string address = m[1];
	ba::replace_all(address, ",", ".");
	
	string port = boost::lexical_cast<string>(
		boost::lexical_cast<uint32>(m[2]) * 256 +
		boost::lexical_cast<uint32>(m[3]));

	// Get a list of endpoints corresponding to the server name.
	at::tcp::resolver::query query(address, port);
	at::tcp::resolver::iterator endpoint_iterator = mResolver.resolve(query);
	at::tcp::resolver::iterator end;

	at::tcp::socket socket(mIOService);

	// Try each endpoint until we successfully establish a connection.
	boost::system::error_code error = boost::asio::error::host_not_found;
    while (error and endpoint_iterator != end)
    {
		socket.close();
		socket.connect(*endpoint_iterator++, error);
	}

	if (error)
		throw boost::system::system_error(error);

    boost::asio::streambuf response;
		
	// Yeah, we have a data connection, now send the RETR command
	string remote = inRemote.string();
	ba::replace_all(remote, "\\", "/");
	status = SendAndWaitForReply("retr", remote);

	if (status != 125 and status != 150)
		Error("Error retrieving file");
	
	while (size_t r = boost::asio::read(socket, response, boost::asio::transfer_at_least(1), error))
	{
		file << &response;
		inProgress.Consumed(r);
	}

    if (error != boost::asio::error::eof)
		throw boost::system::system_error(error);
	
	status = WaitForReply();
	if (status != 226)
		Error("Error retrieving file");
	
	file.close();
	if (fs::exists(inLocal))
		fs::remove(inLocal);
	
	fs::rename(local, inLocal);
	if (inTime > 0)
	{
		fs::last_write_time(inLocal, inTime, error);
		if (error)
			cerr << "Error setting time on newly created file" << endl;
	}
}

// --------------------------------------------------------------------

struct M6RSyncFetcherImpl : public M6FetcherImpl
{
						M6RSyncFetcherImpl(const zx::element* inConfig);
	
	virtual void		Mirror(bool inDryRun, ostream& out);
};

M6RSyncFetcherImpl::M6RSyncFetcherImpl(const zx::element* inConfig)
	: M6FetcherImpl(inConfig)
{
}

void M6RSyncFetcherImpl::Mirror(bool inDryRun, ostream& out)
{
	string databank = mConfig->get_attribute("id");
	
	M6Progress progress(databank, "rsync");

	zx::element* source = mConfig->find_first("source");
	string srcdir = source->content();

	string fetch = source->get_attribute("fetch");
	
	string rsync = M6Config::GetTool("rsync");
	if (not fs::exists(rsync))
		THROW(("rsync not found"));
	
	vector<const char*> args;
	args.push_back(rsync.c_str());
	args.push_back("-ltpvd");
	if (source->get_attribute("recursive") == "true")
		args.push_back("--recursive");
	if (source->get_attribute("delete") == "true")
		args.push_back("--delete");
	if (inDryRun)
		args.push_back("--dry-run");
	args.push_back(fetch.c_str());
	
	bool stripped = false;
	if (not ba::ends_with(srcdir, "/"))
	{
		// strip off the pattern to match files... just rsync everything
		boost::regex rx("/([^/*?{]*[*?{][^/]*)$");
		boost::smatch m;
		
		while (boost::regex_search(srcdir, m, rx))
		{
			stripped = true;
			srcdir = m.prefix();
		}
	}
	
	if (stripped)
	{
		if (srcdir.empty())
			srcdir = databank;
		if (not ba::ends_with(srcdir, "/"))
			srcdir += '/';
	}
	else
		srcdir = source->content();	// probably a full path to a single file?
	
	srcdir = (fs::path(M6Config::GetDirectory("raw")) / srcdir).string();
	if (ba::contains(srcdir, "/../"))
		THROW(("invalid destination path for rsync"));

	args.push_back(srcdir.c_str());
	args.push_back(nullptr);
	
	for_each(args.begin(), args.end(), [](const char* arg) { if (arg != nullptr) cout << arg << ' '; });
	cout << endl;
	
	stringstream in;
	int r = ForkExec(args, 0, in, cout, cerr);
	
	if (r != 0)
		THROW(("Failed to rsync %s", databank.c_str()));
}

// --------------------------------------------------------------------

class M6Fetcher
{
  public:
						M6Fetcher(const zx::element* inConfig);
						~M6Fetcher();
	
	void				Mirror(bool inDryRun, ostream& out);

  private:
	M6FetcherImpl*		mImpl;
};

M6Fetcher::M6Fetcher(const zx::element* inConfig)
	: mImpl(nullptr)
{
	zx::element* source = inConfig->find_first("source");
	if (source == nullptr)
		THROW(("Missing source?"));
	
	string fetch = source->get_attribute("fetch");
	if (ba::starts_with(fetch, "rsync://"))
		mImpl = new M6RSyncFetcherImpl(inConfig);
	else if (ba::starts_with(fetch, "ftp://"))
		mImpl = new M6FTPFetcherImpl(inConfig);
	else
		THROW(("Unsupported URL method in %s", fetch.c_str()));
}

M6Fetcher::~M6Fetcher()
{
	delete mImpl;
}

void M6Fetcher::Mirror(bool inDryRun, ostream& out)
{
	if (mImpl != nullptr)
		mImpl->Mirror(inDryRun, out);
}

void M6Fetch(const string& inDatabank)
{
	const zx::element* config = M6Config::GetEnabledDatabank(inDatabank);
	M6Fetcher fetch(config);
	fetch.Mirror(false, cout);
}

void M6DryRunFetch(const std::string& inDatabank, ostream& outResult)
{
	const zx::element* config = M6Config::GetEnabledDatabank(inDatabank);
	M6Fetcher fetch(config);
	fetch.Mirror(true, outResult);
}
