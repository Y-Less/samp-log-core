#pragma once
#ifndef INC_SAMPLOG_LOGGER_H
#define INC_SAMPLOG_LOGGER_H

#include "LogLevel.h"
#include "DebugInfo.h"
#include <stddef.h>

//NOTE: Passing "-fvisibility=hidden" as a compiler option to GCC is advised!
#if defined _WIN32 || defined __CYGWIN__
# ifdef __GNUC__
#  define DLL_PUBLIC __attribute__ ((dllimport))
# else
#  define DLL_PUBLIC __declspec(dllimport)
# endif
#else
# if __GNUC__ >= 4
#  define DLL_PUBLIC __attribute__ ((visibility ("default")))
# else
#  define DLL_PUBLIC
# endif
#endif


extern "C" DLL_PUBLIC samplog_LogLevel samplog_GetLogLevel(const char *module);
extern "C" DLL_PUBLIC void samplog_SetLogLevel(const char *module, samplog_LogLevel level);
extern "C" DLL_PUBLIC bool samplog_Register(const char *module);
extern "C" DLL_PUBLIC void samplog_Init();
extern "C" DLL_PUBLIC void samplog_Exit();
extern "C" DLL_PUBLIC bool samplog_LogMessage(
	const char *module, samplog_LogLevel level, const char *msg,
	samplog_AmxFuncCallInfo const *call_info = NULL,
	unsigned int call_info_size = 0);


#ifdef __cplusplus

#include <string>
#include <vector>

namespace samplog
{
	class CLoggerExists
	{
	public:
		explicit CModuleExists(std::string modulename) :
			m_Module(std::move(modulename))
		{ }
		
		const char* what() const noexcept { return ("Module \"" + m_Module + "\" already exists").c_str(); }
	private:
		std::string m_Module;
	}
	
	inline void Init()
	{
		samplog_Init();
	}
	inline void Exit()
	{
		samplog_Exit();
	}
	inline bool LogMessage(
		const char *module, LogLevel level, const char *msg,
		samplog_AmxFuncCallInfo const *call_info = nullptr,
		unsigned int call_info_size = 0)
	{
		return samplog_LogMessage(module, level, msg, call_info, call_info_size);
	}
	
	class CLogger
	{
	public:
		explicit CLogger(std::string modulename) :
			m_Module(std::move(modulename))
		{
			if (!samplog_Register(m_Module.c_str()))
				throw CLoggerExists(m_Module);
		}
		~CLogger() = default;
		CLogger(CLogger const &rhs) = delete;
		CLogger& operator=(CLogger const &rhs) = delete;

		CLogger(CLogger &&other) = delete;
		CLogger& operator=(CLogger &&other) = delete;

	public:
		inline void SetLogLevel(LogLevel log_level)
		{
			samplog_SetLogLevel(m_Module.c_str(), log_level);
		}
		inline bool IsLogLevel(LogLevel log_level) const
		{
			return (samplog_GetLogLevel(m_Module.c_str()) & log_level) == log_level;
		}

		inline bool Log(LogLevel level, const char *msg,
			std::vector<AmxFuncCallInfo> const &call_info)
		{
			if (!IsLogLevel(level))
				return false;

			return samplog::LogMessage(m_Module.c_str(), level, msg, 
				call_info.data(), call_info.size());
		}

		inline bool Log(LogLevel level, const char *msg)
		{
			if (!IsLogLevel(level))
				return false;

			return samplog::LogMessage(m_Module.c_str(), level, msg);
		}

	protected:
		std::string m_Module;
	};

	typedef CLogger Logger_t;

}

#endif /* __cplusplus */

#undef DLL_PUBLIC

#endif /* INC_SAMPLOG_LOGGER_H */
