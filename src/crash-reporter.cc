
#include <iostream>
#include <fstream>
#include <iomanip>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/filesystem.hpp>

#include <memory>

#include "crash-reporter.hpp"

using boost::asio::ip::tcp;

#ifdef SENTRY_HOST_NAME
const std::string host = SENTRY_HOST_NAME
#else
const std::string host = "sentry.io";
//const std::string host = "127.0.0.1";
#endif

const std::string protocol = "https";
const std::string api_path = "/api/1390326/minidump/?sentry_key=7492ebea21f54618a550163938dc164d";
const bool send_manual_backtrace = false;

extern bool update_completed;
bool report_unsuccessful = false;

std::time_t app_start_timestamp;

std::string get_uuid() noexcept;
std::string get_timestamp() noexcept;

double get_time_from_start() noexcept;

std::string prepare_crash_report(struct _EXCEPTION_POINTERS* ExceptionInfo) noexcept;
int send_crash_to_sentry_sync(const std::string& report_json) noexcept;

void save_start_timestamp();
std::string get_command_line() noexcept;

void handle_exit() noexcept;
void handle_crash(struct _EXCEPTION_POINTERS* ExceptionInfo, bool callAbort = true) noexcept;

struct _EXCEPTION_POINTERS* generate_exception_info() noexcept;
void print_stacktrace_sym(CONTEXT* ctx, std::ostringstream & report_stream) noexcept; //Prints stack trace based on context record

void create_mini_dump(EXCEPTION_POINTERS* pep) noexcept;

std::string escapeJsonString(const std::string& input) noexcept;

#include "dbghelp.h"
#pragma comment(lib,"Dbghelp.lib")

std::string prepare_crash_report(struct _EXCEPTION_POINTERS* ExceptionInfo) noexcept
{
	std::ostringstream json_report;

	json_report << "{";
	json_report << "	\"event_id\": \"" << get_uuid() << "\", ";
	json_report << "	\"timestamp\": \"" << get_timestamp() << "\", ";
	if (send_manual_backtrace)
	{
		json_report << "	\"exception\": {\"values\":[{";
		if (ExceptionInfo)
		{
			json_report << "		\"type\": \"" << ExceptionInfo->ExceptionRecord->ExceptionCode << "\", ";
		}
		//	json_report << "		\"value\": \"" << "ERROR_VALUE" << "\", ";
		//	json_report << "		\"module\": \"" << "MODULE_NAME" << "\", ";
		json_report << "		\"thread_id\": \"" << std::this_thread::get_id() << "\", ";

		if (ExceptionInfo)
		{
			std::string method;
			json_report << "		\"stacktrace\": { \"frames\" : [";
			print_stacktrace_sym(ExceptionInfo->ContextRecord, json_report);
			json_report << "		] } ";
		}
		json_report << "	}]}, ";
	}
	json_report << "	\"tags\": { ";
	json_report << "		\"app_build_timestamp\": \"" << __DATE__ << " " << __TIME__ << "\", ";
	json_report << "		\"release\": \"" << "0.0.1" << "\", ";
	json_report << "		\"os_version\": \"" << "WIN32" << "\" ";
	json_report << "	}, ";
	json_report << "	\"extra\": { ";
	json_report << "		\"app_run_time\": \"" << get_time_from_start() << "\", ";
	json_report << "		\"console_args\": \"" << get_command_line() << "\" ";
	json_report << "	} ";
	json_report << "}";

	return json_report.str();
}

void create_mini_dump(EXCEPTION_POINTERS* pep) noexcept
{
	// Open the file 
	HANDLE hFile = CreateFile(L"MiniDump.dmp", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if ((hFile != NULL) && (hFile != INVALID_HANDLE_VALUE))
	{
		MINIDUMP_EXCEPTION_INFORMATION mdei;

		mdei.ThreadId = GetCurrentThreadId();
		mdei.ExceptionPointers = pep;
		mdei.ClientPointers = FALSE;

		const DWORD Flags = MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpFilterMemory | MiniDumpScanMemory;

		BOOL rv = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, (MINIDUMP_TYPE)Flags, (pep != 0) ? &mdei : 0, 0, 0);

		CloseHandle(hFile);
	}
	else {

	}
}

int send_crash_to_sentry_sync(const std::string& report_json) noexcept
{
	try
	{
		boost::asio::ssl::context context(boost::asio::ssl::context::sslv23);

		context.set_default_verify_paths();

		boost::asio::io_service io_service;
		boost::asio::ssl::stream<boost::asio::ip::tcp::socket> ssl_socket(io_service, context);


		tcp::resolver resolver(io_service);
		tcp::resolver::query query(host, protocol);
		tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);
		tcp::resolver::iterator end;

		tcp::socket socket(io_service);
		boost::system::error_code error = boost::asio::error::host_not_found;
		while (error && endpoint_iterator != end)
		{
			ssl_socket.lowest_layer().close();
			boost::asio::connect(ssl_socket.lowest_layer(), endpoint_iterator, error);

			if (!error)
			{
				ssl_socket.set_verify_mode(boost::asio::ssl::verify_none);
				ssl_socket.handshake(boost::asio::ssl::stream_base::client);
				break;
			}
			endpoint_iterator++;
		}
		if (error)
		{
			throw boost::system::system_error(error);
		}

		const std::string filename("MiniDump.dmp");
		long long fileSize = boost::filesystem::file_size(filename);

		const std::string PREFIX = "--";
		const std::string BOUNDARY = get_uuid();
		const std::string NEWLINE = "\r\n";
		const int NEWLINE_LENGTH = NEWLINE.length();

		//Calculate length of entire HTTP request - goes into header
		long long lengthOfRequest = 0;
		lengthOfRequest += PREFIX.length() + BOUNDARY.length() + NEWLINE_LENGTH;
		lengthOfRequest += std::string("Content-Disposition: form-data; name=\"upload_file_minidump\"; filename=\"MiniDump.dmp\"").length();
		lengthOfRequest += NEWLINE_LENGTH + NEWLINE_LENGTH;
		lengthOfRequest += fileSize;
		lengthOfRequest += NEWLINE_LENGTH + PREFIX.length() + BOUNDARY.length() + NEWLINE_LENGTH;

		lengthOfRequest += std::string("Content-Disposition: form-data; name=\"sentry\"").length();
		lengthOfRequest += NEWLINE_LENGTH + NEWLINE_LENGTH;
		lengthOfRequest += report_json.size();
		lengthOfRequest += NEWLINE_LENGTH + PREFIX.length() + BOUNDARY.length() + PREFIX.length() + NEWLINE_LENGTH;// + NEWLINE_LENGTH;

		FILE * filePointer;
		fopen_s(&filePointer, filename.c_str(), "rb");
		std::unique_ptr<unsigned char[]> charArray(new unsigned char[1024 * 1024 * 2]);
		fseek(filePointer, 0, SEEK_SET);
		fread_s(charArray.get(), 1024 * 1024 * 2, sizeof(unsigned char), fileSize, filePointer);


		boost::asio::streambuf request;
		std::ostream request_stream(&request);

		request_stream << "POST " << api_path << " HTTP/1.1" << NEWLINE;
		request_stream << "Host: " << host << NEWLINE;
		request_stream << "Accept: *" << NEWLINE;
		request_stream << "User-Agent: slobs_updater " << NEWLINE;
		request_stream << "Accept-encoding: 'gzip, deflate, br'" << NEWLINE;
		request_stream << "Accept-language: 'en-US,en;q=0.9,ru;q=0.8'" << NEWLINE;
		request_stream << "Connection: close" << NEWLINE;
		request_stream << "X-Sentry-Auth: Sentry sentry_version=5,sentry_client=slobs_updater/";
		request_stream << "1.0.0" << ",sentry_timestamp=" << get_timestamp();
		request_stream << ",sentry_key=" << "7492ebea21f54618a550163938dc164d";
		request_stream << ",sentry_secret=" << "654ed1db5d93495284f66f3d6d195790";
		request_stream << NEWLINE;

		request_stream << "Content-Length: " << lengthOfRequest << NEWLINE;
		request_stream << "Content-Type: multipart/form-data; boundary=" << BOUNDARY;
		request_stream << NEWLINE << NEWLINE;
		request_stream << PREFIX << BOUNDARY;
		request_stream << NEWLINE;
		request_stream << "Content-Disposition: form-data; name=\"upload_file_minidump\"; filename=\"MiniDump.dmp\"";
		request_stream << NEWLINE << NEWLINE;

		// Send the request.
		boost::asio::write(ssl_socket, request);

		auto bytesSent = 0;
		while (bytesSent < fileSize)
		{
			int bytesToSendNow = fileSize - bytesSent;
			if (bytesToSendNow > 1024 * 10) bytesToSendNow = 1024 * 10;
			boost::asio::write(ssl_socket, boost::asio::buffer(charArray.get() + bytesSent, bytesToSendNow));

			bytesSent += bytesToSendNow;
		}

		boost::asio::streambuf request_end;
		std::ostream request_stream_end(&request_end);
		request_stream_end << NEWLINE;
		request_stream_end << PREFIX << BOUNDARY;
		request_stream_end << NEWLINE;
		request_stream_end << "Content-Disposition: form-data; name=\"sentry\"";
		request_stream_end << NEWLINE << NEWLINE;
		request_stream_end << report_json;
		request_stream_end << NEWLINE;
		request_stream_end << PREFIX << BOUNDARY;
		request_stream_end << PREFIX;
		request_stream_end << NEWLINE;
		//request_stream_end << NEWLINE;
		boost::asio::write(ssl_socket, request_end);


		// Read the response status line. 
		boost::asio::streambuf response;
		boost::asio::read_until(ssl_socket, response, "\r\n");

		// Check that response is OK.
		std::istream response_stream(&response);
		std::string http_version;
		response_stream >> http_version;
		unsigned int status_code;
		response_stream >> status_code;

		std::string status_message;
		std::getline(response_stream, status_message);

		if (!response_stream || http_version.substr(0, 5) != "HTTP/")
		{
		}
		else
		{
			// Read the response headers, which are terminated by a blank line.
			boost::asio::read_until(ssl_socket, response, "\r\n\r\n");

			// Process the response headers. 
			// Not much we can do if sentry response with error. 
			// Just read data to make connection finish correctly
			std::string header;
			while (std::getline(response_stream, header) && header != "\r\n")
			{
			}

			if (response.size() > 0)
			{

			}

			// Read until EOF, checking data as we go.
			while (boost::asio::read(ssl_socket, response, boost::asio::transfer_at_least(1), error))
			{

			}

			if (error != boost::asio::error::eof)
			{
				throw boost::system::system_error(error);
			}
		}

	}
	catch (std::exception& e)
	{
		//have to ignore exceptions as programm is in reporting exception already
	}


	return 0;
}

void handle_crash(struct _EXCEPTION_POINTERS* ExceptionInfo, bool callAbort) noexcept
{
	static bool insideCrashMethod = false;
	if (insideCrashMethod)
	{
		abort();
	}
	insideCrashMethod = true;

	std::string report = prepare_crash_report(ExceptionInfo);

	create_mini_dump(ExceptionInfo);

	send_crash_to_sentry_sync(report);

	DeleteFile(L"MiniDump.dmp");

	if (callAbort)
	{
		abort();
	}

	insideCrashMethod = false;
}

void handle_exit() noexcept
{
	if (!update_completed && report_unsuccessful)
	{
		handle_crash(generate_exception_info(), false);
	}
}

struct _EXCEPTION_POINTERS* generate_exception_info() noexcept
{
	//todo
	return nullptr;
}

void print_stacktrace_sym(CONTEXT* ctx, std::ostringstream & report_stream) noexcept
{
	BOOL    result;
	HANDLE  process;
	HANDLE  thread;
	HMODULE hModule;

	STACKFRAME64 stack;
	ULONG        frame;
	DWORD64      displacement;

	DWORD			disp;
	IMAGEHLP_LINE64 *line;
	const int		MaxNameLen = 256;

	char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
	char name[MaxNameLen];
	char module[MaxNameLen];
	PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;

	memset(&stack, 0, sizeof(STACKFRAME64));

	process = GetCurrentProcess();
	thread = GetCurrentThread();
	displacement = 0;
#if !defined(_M_AMD64)
	stack.AddrPC.Offset = (*ctx).Eip;
	stack.AddrPC.Mode = AddrModeFlat;
	stack.AddrStack.Offset = (*ctx).Esp;
	stack.AddrStack.Mode = AddrModeFlat;
	stack.AddrFrame.Offset = (*ctx).Ebp;
	stack.AddrFrame.Mode = AddrModeFlat;
#endif

	SymInitialize(process, NULL, TRUE);
	bool first_element = true;
	for (frame = 0; ; frame++)
	{
		//get next call from stack
		result = StackWalk64
		(
#if defined(_M_AMD64)
			IMAGE_FILE_MACHINE_AMD64
#else
			IMAGE_FILE_MACHINE_I386
#endif
			, process, thread, &stack, ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL);

		if (!result) break;

		if (!first_element)
		{
			report_stream << ",";
		}
		report_stream << " { ";

		//get symbol name for address
		pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		pSymbol->MaxNameLen = MAX_SYM_NAME;
		if (SymFromAddr(process, (ULONG64)stack.AddrPC.Offset, &displacement, pSymbol))
		{
			line = (IMAGEHLP_LINE64 *)malloc(sizeof(IMAGEHLP_LINE64));
			line->SizeOfStruct = sizeof(IMAGEHLP_LINE64);



			report_stream << " 	\"function\": \"" << pSymbol->Name << "\", ";
			report_stream << " 	\"instruction_addr\": \"" << "0x" << std::uppercase << std::setfill('0') << std::setw(12) << std::hex << pSymbol->Address << "\", ";
			if (SymGetLineFromAddr64(process, stack.AddrPC.Offset, &disp, line))
			{
				report_stream << " 	\"lineno\": \"" << line->LineNumber << "\", ";
				std::string file_name = line->FileName;
				file_name = escapeJsonString(file_name);
				report_stream << " 	\"filename\": \"" << file_name << "\", ";
			}
			else
			{
				//failed to get line number
			}
			free(line);
			line = NULL;
		}
		else
		{
			report_stream << " 	\"function\": \"" << "unknown" << "\", ";
			report_stream << " 	\"instruction_addr\": \"" << "0x" << std::uppercase << std::setfill('0') << std::setw(12) << std::hex << (ULONG64)stack.AddrPC.Offset << "\", ";
		}

		hModule = NULL;
		lstrcpyA(module, "");
		if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCTSTR)(stack.AddrPC.Offset), &hModule))
		{
			if (hModule != NULL)
			{
				if (GetModuleFileNameA(hModule, module, MaxNameLen))
				{
					std::string module_name = module;
					module_name = escapeJsonString(module_name);
					report_stream << " 	\"module\": \"" << module_name << "\" ";
				}
			}
		}

		first_element = false;
		report_stream << " } ";
	}
}

void save_start_timestamp()
{
	std::time(&app_start_timestamp);
}

double get_time_from_start() noexcept
{
	time_t current_time;
	time(&current_time);
	return difftime(current_time, app_start_timestamp);
}

std::string get_command_line() noexcept
{
	std::string ret = "empty";
	LPWSTR lpCommandLine = GetCommandLine();
	if (lpCommandLine != nullptr)
	{
		std::wstring ws_args = std::wstring(lpCommandLine);
		ret = std::string(ws_args.begin(), ws_args.end());
		ret = escapeJsonString(ret);
	}
	return ret;
}

void setup_crash_reporting()
{
	save_start_timestamp();

	std::set_terminate([]() { handle_crash(nullptr); });

	SetUnhandledExceptionFilter([](struct _EXCEPTION_POINTERS* ExceptionInfo)
	{
		/* don't use if a debugger is present */
		if (IsDebuggerPresent())
		{
			return LONG(EXCEPTION_CONTINUE_SEARCH);
		}

		handle_crash(ExceptionInfo);

		// Unreachable statement
		return LONG(EXCEPTION_CONTINUE_SEARCH);
	});

	// The atexit will check if updater was safelly closed
	std::atexit(handle_exit);
	std::at_quick_exit(handle_exit);

}

std::string get_timestamp() noexcept
{
	char buf[sizeof "xxxx-xx-xxTxx:xx:xxx\0"];
	std::time_t curent_time;

	std::time(&curent_time);
	std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&curent_time));

	return std::string(buf);
}

std::string get_uuid() noexcept
{
	char result[33] = { '\0' }; //"fc6d8c0c43fc4630ad850ee518f1b9d0";
	std::srand(std::time(nullptr));

	for (std::size_t i = 0; i < sizeof(result) - 1; ++i)
	{
		const auto r = static_cast<char>(std::rand() % 16);
		if (r < 10)
		{
			result[i] = '0' + r;
		}
		else
		{
			result[i] = 'a' + r - static_cast<char>(10);
		}
	}

	return std::string(result);
}

std::string escapeJsonString(const std::string& input) noexcept
{
	std::ostringstream ss;
	for (auto iter = input.cbegin(); iter != input.cend(); iter++)
	{
		switch (*iter)
		{
		case '\\': ss << "\\\\"; break;
		case '"': ss << "\\\""; break;
		case '/': ss << "\\/"; break;
		case '\b': ss << "\\b"; break;
		case '\f': ss << "\\f"; break;
		case '\n': ss << "\\n"; break;
		case '\r': ss << "\\r"; break;
		case '\t': ss << "\\t"; break;
		default: ss << *iter; break;
		}
	}
	return ss.str();
}