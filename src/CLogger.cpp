#include <algorithm>
#include <fstream>
#include <ctime>
#include <set>

#ifdef WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#else
#  include <sys/stat.h>
#endif

#include "CLogger.hpp"
#include "CSampConfigReader.hpp"
#include "crashhandler.hpp"
#include "amx/amx2.h"

#include <fmt/format.h>
#include <fmt/time.h>


CLogManager::CLogManager() :
	m_ThreadRunning(true),
	m_DateTimeFormat("{:%x %X}")
{
	crashhandler::Install();

	std::string cfg_time_format;
	if (CSampConfigReader::Get()->GetVar("logtimeformat", cfg_time_format))
	{
		//delete brackets
		size_t pos = 0;
		while ((pos = cfg_time_format.find_first_of("[]()")) != std::string::npos)
			cfg_time_format.erase(pos, 1);

		m_DateTimeFormat = "{:" + cfg_time_format + "}";
		// quickly test out the format string
		// will assert if invalid and on Windows
		fmt::format(m_DateTimeFormat, fmt::localtime(std::time(nullptr)));
	}

	CreateFolder("logs");

	m_WarningLog.open("logs/warnings.log");
	m_ErrorLog.open("logs/errors.log");

	m_Thread = new std::thread(std::bind(&CLogManager::Process, this));
}

CLogManager::~CLogManager()
{
	{
		std::lock_guard<std::mutex> lg(m_QueueMtx);
		m_ThreadRunning = false;
	}
	m_QueueNotifier.notify_one();
	m_Thread->join();
	delete m_Thread;
}

void CLogManager::QueueLogMessage(Message_t &&msg)
{
	{
		std::lock_guard<std::mutex> lg(m_QueueMtx);
		m_LogMsgQueue.push(std::move(msg));
	}
	m_QueueNotifier.notify_one();
}

void CLogManager::Process()
{
	std::unique_lock<std::mutex> lk(m_QueueMtx);
	std::set<size_t> HashedModules;
	std::hash<std::string> StringHash;

	do
	{
		m_QueueNotifier.wait(lk);
		while (!m_LogMsgQueue.empty())
		{
			Message_t msg = std::move(m_LogMsgQueue.front());
			m_LogMsgQueue.pop();

			//manually unlock mutex
			//the whole write-to-file code below has no need to be locked with the
			//message queue mutex; while writing to the log file, new messages can
			//now be queued
			lk.unlock();

			std::string timestamp;
			std::time_t now_c = std::chrono::system_clock::to_time_t(msg->timestamp);
			timestamp = fmt::format(m_DateTimeFormat, fmt::localtime(now_c));

			const char *loglevel_str = "<unknown>";
			switch (msg->loglevel)
			{
			case LogLevel::DEBUG:
				loglevel_str = "DEBUG";
				break;
			case LogLevel::INFO:
				loglevel_str = "INFO";
				break;
			case LogLevel::WARNING:
				loglevel_str = "WARNING";
				break;
			case LogLevel::ERROR:
				loglevel_str = "ERROR";
				break;
			}

			const string &modulename = msg->log_module;
			size_t module_hash = StringHash(modulename);
			if (HashedModules.find(module_hash) == HashedModules.end())
			{
				//create possibly non-existing folders before opening log file
				size_t pos = 0;
				while ((pos = modulename.find('/', pos)) != std::string::npos)
				{
					CreateFolder("logs/" + modulename.substr(0, pos++));
				}

				HashedModules.insert(module_hash);
			}

			// build log string
			fmt::MemoryWriter log_string;

			log_string << msg->text;
			if (!msg->call_info.empty())
			{
				log_string << " (";
				bool first = true;
				for (auto const &ci : msg->call_info)
				{
					if (!first)
						log_string << " -> ";
					log_string << ci.file << ":" << ci.line;
					first = false;
				}
				log_string << ")";
			}

			//default logging
			std::ofstream logfile("logs/" + modulename + ".log",
				std::ofstream::out | std::ofstream::app);
			logfile <<
				"[" << timestamp << "] " <<
				"[" << loglevel_str << "] " <<
				log_string.str() << '\n' << std::flush;


			//per-log-level logging
			std::ofstream *loglevel_file = nullptr;
			if (msg->loglevel & LogLevel::WARNING)
				loglevel_file = &m_WarningLog;
			else if (msg->loglevel & LogLevel::ERROR)
				loglevel_file = &m_ErrorLog;

			if (loglevel_file != nullptr)
			{
				(*loglevel_file) <<
					"[" << timestamp << "] " <<
					"[" << modulename << "] " <<
					log_string.str() << '\n' << std::flush;
			}

			//lock the log message queue again (because while-condition and cv.wait)
			lk.lock();
		}
	} while (m_ThreadRunning);
}

void CLogManager::CreateFolder(std::string foldername)
{
#ifdef WIN32
	std::replace(foldername.begin(), foldername.end(), '/', '\\');
	CreateDirectoryA(foldername.c_str(), NULL);
#else
	std::replace(foldername.begin(), foldername.end(), '\\', '/');
	mkdir(foldername.c_str(), ACCESSPERMS);
#endif
}


void samplog_Init()
{
	CLogManager::Get()->IncreasePluginCounter();
}

void samplog_Exit()
{
	CLogManager::Get()->DecreasePluginCounter();
}

bool samplog_LogMessage(const char *module, LogLevel level, const char *msg,
	samplog_AmxFuncCallInfo const *call_info /*= NULL*/, unsigned int call_info_size /*= 0*/)
{
	if (module == nullptr || strlen(module) == 0)
		return false;

	std::vector<AmxFuncCallInfo> my_call_info;
	if (call_info != nullptr && call_info_size != 0)
	{
		for (unsigned int i = 0; i != call_info_size; ++i)
			my_call_info.push_back(call_info[i]);
	}

	CLogManager::Get()->QueueLogMessage(std::unique_ptr<CMessage>(new CMessage(
		module, level, msg ? msg : "", std::move(my_call_info))));
	return true;
}

bool samplog_LogNativeCall(const char *module,
	AMX * const amx, cell * const params, const char *name, const char *params_format)
{
	if (module == nullptr || strlen(module) == 0)
		return false;

	if (amx == nullptr)
		return false;

	if (params == nullptr)
		return false;

	if (name == nullptr || strlen(name) == 0)
		return false;

	if (params_format == nullptr) // params_format == "" is valid (no parameters)
		return false;


	size_t format_len = strlen(params_format);

	fmt::MemoryWriter fmt_msg;
	fmt_msg << name << '(';

	for (int i = 0; i != format_len; ++i)
	{
		if (i != 0)
			fmt_msg << ", ";

		cell current_param = params[i + 1];
		switch (params_format[i])
		{
		case 'd': //decimal
		case 'i': //integer
			fmt_msg << static_cast<int>(current_param);
			break;
		case 'f': //float
			fmt_msg << amx_ctof(current_param);
			break;
		case 'h': //hexadecimal
		case 'x': //
			fmt_msg << fmt::hex(current_param);
			break;
		case 'b': //binary
			fmt_msg << fmt::bin(current_param);
			break;
		case 's': //string
			fmt_msg << '"' << amx_GetCppString(amx, current_param) << '"';
			break;
		case '*': //censored output
			fmt_msg << "\"*****\"";
			break;
		case 'r': //reference
		{
			cell *addr_dest = nullptr;
			amx_GetAddr(amx, current_param, &addr_dest);
			fmt_msg << "0x" << fmt::pad(fmt::hexu(reinterpret_cast<unsigned int>(addr_dest)), 8, '0');
		}	break;
		case 'p': //pointer-value
			fmt_msg << "0x" << fmt::pad(fmt::hexu(current_param), 8, '0');
			break;
		default:
			return false; //unrecognized format specifier
		}
	}
	fmt_msg << ')';

	std::vector<AmxFuncCallInfo> call_info;
	CAmxDebugManager::Get()->GetFunctionCallTrace(amx, call_info);

	CLogManager::Get()->QueueLogMessage(std::unique_ptr<CMessage>(new CMessage(
		module, LogLevel::DEBUG, fmt_msg.str(), std::move(call_info))));

	return true;
}
