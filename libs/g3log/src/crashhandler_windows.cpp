/** ==========================================================================
 * 2011 by KjellKod.cc. This is PUBLIC DOMAIN to use at your own risk and comes
 * with no warranties. This code is yours to share, use and modify with no
 * strings attached and no restrictions or obligations.
 *
 * For more information see g3log/LICENSE or refer refer to http://unlicense.org
 * ============================================================================*/

#if !(defined(WIN32) || defined(_WIN32) || defined(__WIN32__))
#error "crashhandler_windows.cpp used but not on a windows system"
#endif

#include <windows.h>
#include <intrin.h>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <atomic>
#include <process.h> // getpid
#include "crashhandler.hpp"
#include "stacktrace_windows.hpp"
#include "g2logmessage.hpp"

#define getpid _getpid

// thread_local doesn't exist on VS2013 but it might soon? (who knows)
// to work after Microsoft has updated to be C++11 compliant
#if !(defined(thread_local))
#define thread_local __declspec(thread) 
#endif 

namespace {
std::atomic<bool> gBlockForFatal {true};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_unexpected_exception_handler = nullptr;

thread_local bool g_installed_thread_signal_handler = false;

#if !(defined(DISABLE_VECTORED_EXCEPTIONHANDLING))
   void* g_vector_exception_handler = nullptr;
#endif



// Restore back to default fatal event handling
void ReverseToOriginalFatalHandling() {
   SetUnhandledExceptionFilter (g_previous_unexpected_exception_handler);

#if !(defined(DISABLE_VECTORED_EXCEPTIONHANDLING))
   RemoveVectoredExceptionHandler (g_vector_exception_handler);
#endif

   if (SIG_ERR == signal(SIGABRT, SIG_DFL))
      perror("signal - SIGABRT");

   if (SIG_ERR == signal(SIGFPE, SIG_DFL))
      perror("signal - SIGABRT");

   if (SIG_ERR == signal(SIGSEGV, SIG_DFL))
      perror("signal - SIGABRT");

   if (SIG_ERR == signal(SIGILL, SIG_DFL))
      perror("signal - SIGABRT");

   if (SIG_ERR == signal(SIGTERM, SIG_DFL))
      perror("signal - SIGABRT");
}



// called for fatal signals SIGABRT, SIGFPE, SIGSEGV, SIGILL, SIGTERM
void signalHandler(int signal_number) {
   using namespace g2::internal;
   std::string dump = stacktrace::stackdump();

   std::ostringstream fatal_stream;
   fatal_stream << "\n***** Received fatal signal " << g2::internal::exitReasonName(LOGLEVEL::FATAL_SIGNAL, signal_number);
   fatal_stream << "(" << signal_number << ")\tPID: " << getpid() << std::endl;

   /* //TODO: notify all loggers
   LogCapture trigger(nullptr, LOGLEVEL::FATAL_SIGNAL, static_cast<g2::SignalType>(signal_number), dump.c_str());
   trigger.stream() << fatal_stream.str();
   */
}



// Unhandled exception catching
LONG WINAPI exceptionHandling(EXCEPTION_POINTERS* info, const std::string& handler) {
   std::string dump = stacktrace::stackdump(info);

   std::ostringstream fatal_stream;
   const g2::SignalType exception_code = info->ExceptionRecord->ExceptionCode;
   fatal_stream << "\n***** " << handler << ": Received fatal exception " << g2::internal::exitReasonName(LOGLEVEL::FATAL_EXCEPTION, exception_code);
   fatal_stream << "\tPID: " << getpid() << std::endl;

   /* //TODO: notify all loggers
   const auto fatal_id = static_cast<g2::SignalType>(exception_code);
   LogCapture trigger(nullptr, LOGLEVEL::FATAL_EXCEPTION, fatal_id, dump.c_str());
   trigger.stream() << fatal_stream.str();
   */
   // FATAL Exception: It doesn't necessarily stop here we pass on continue search
   // if no one else will catch that then it's goodbye anyhow.
   // The RISK here is if someone is cathing this and returning "EXCEPTION_EXECUTE_HANDLER"
   // but does not shutdown then the software will be running with g3log shutdown.
   // .... However... this must be seen as a bug from standard handling of fatal exceptions
   // https://msdn.microsoft.com/en-us/library/6wxdsc38.aspx
   return EXCEPTION_CONTINUE_SEARCH;
}


// Unhandled exception catching
LONG WINAPI unexpectedExceptionHandling(EXCEPTION_POINTERS* info) {
   ReverseToOriginalFatalHandling();
   return exceptionHandling(info, "Unexpected Exception Handler");
}


/// Setup through (Windows API) AddVectoredExceptionHandler
/// Ref: http://blogs.msdn.com/b/zhanli/archive/2010/06/25/c-tips-addvectoredexceptionhandler-addvectoredcontinuehandler-and-setunhandledexceptionfilter.aspx
#if !(defined(DISABLE_VECTORED_EXCEPTIONHANDLING))
   LONG WINAPI vectorExceptionHandling(PEXCEPTION_POINTERS p) {
      const g2::SignalType exception_code = p->ExceptionRecord->ExceptionCode;
      if (false == stacktrace::isKnownException(exception_code)) {
         // The unknown exception is ignored. Since it is not a Windows
         // fatal exception generated by the OS we leave the 
         // responsibility to deal with this by the client software.
         return EXCEPTION_CONTINUE_SEARCH;
      } else {
         ReverseToOriginalFatalHandling();
         return exceptionHandling(p, "Vectored Exception Handler");
      }
   }
#endif




} // end anonymous namespace


namespace g2 {
namespace internal {


// For windows exceptions this might ONCE be set to false, in case of a
// windows exceptions and not a signal
bool blockForFatalHandling() {
   return gBlockForFatal;
}


/// Generate stackdump. Or in case a stackdump was pre-generated and
/// non-empty just use that one.   i.e. the latter case is only for
/// Windows and test purposes
std::string stackdump(const char* dump) {
   if (nullptr != dump && !std::string(dump).empty()) {
      return {dump};
   }

   return stacktrace::stackdump();
}



/// string representation of signal ID or Windows exception id
std::string exitReasonName(const LOGLEVEL& level, g2::SignalType fatal_id) {
	if (level == LOGLEVEL::FATAL_EXCEPTION) {
      return stacktrace::exceptionIdToText(fatal_id);
   }

   switch (fatal_id) {
   case SIGABRT: return "SIGABRT"; break;
   case SIGFPE: return "SIGFPE"; break;
   case SIGSEGV: return "SIGSEGV"; break;
   case SIGILL: return "SIGILL"; break;
   case SIGTERM: return "SIGTERM"; break;
   default:
      std::ostringstream oss;
      oss << "UNKNOWN SIGNAL(" << fatal_id << ")";
      return oss.str();
   }
}


// Triggered by g2log::LogWorker after receiving a FATAL trigger
// which is LOG(FATAL), CHECK(false) or a fatal signal our signalhandler caught.
// --- If LOG(FATAL) or CHECK(false) the signal_number will be SIGABRT
void exitWithDefaultSignalHandler(const LOGLEVEL& level, g2::SignalType fatal_signal_id) {

   ReverseToOriginalFatalHandling();
   // For windows exceptions we want to continue the possibility of
   // exception handling now when the log and stacktrace are flushed
   // to sinks. We therefore avoid to kill the preocess here. Instead
   //  it will be the exceptionHandling functions above that
   // will let exception handling continue with: EXCEPTION_CONTINUE_SEARCH
   if (LOGLEVEL::FATAL_EXCEPTION == level) {
      gBlockForFatal = false;
      return;
   }

   // for a sigal however, we exit through that fatal signal
   const int signal_number = static_cast<int>(fatal_signal_id);
   raise(signal_number);
}



void installSignalHandler() {
   g2::installSignalHandlerForThread();
}


} // end g2::internal


///  SIGFPE, SIGILL, and SIGSEGV handling must be installed per thread
/// on Windows. This is automatically done if you do at least one LOG(...) call
/// you can also use this function call, per thread so make sure these three
/// fatal signals are covered in your thread (even if you don't do a LOG(...) call
void installSignalHandlerForThread() {
   if (!g_installed_thread_signal_handler) {
      g_installed_thread_signal_handler = true;
      if (SIG_ERR == signal(SIGTERM, signalHandler))
         perror("signal - SIGTERM");
      if (SIG_ERR == signal(SIGABRT, signalHandler))
         perror("signal - SIGABRT");
      if (SIG_ERR == signal(SIGFPE, signalHandler))
         perror("signal - SIGFPE");
      if (SIG_ERR == signal(SIGSEGV, signalHandler))
         perror("signal - SIGSEGV");
      if (SIG_ERR == signal(SIGILL, signalHandler))
         perror("signal - SIGILL");
   }
}

void installCrashHandler() {
   internal::installSignalHandler();
   g_previous_unexpected_exception_handler = SetUnhandledExceptionFilter(unexpectedExceptionHandling);

#if !(defined(DISABLE_VECTORED_EXCEPTIONHANDLING))
      // const size_t kFirstExceptionHandler = 1;
      // kFirstExeptionsHandler is kept here for documentational purposes.
      // The last exception seems more what we want
      const size_t kLastExceptionHandler = 0;
      g_vector_exception_handler = AddVectoredExceptionHandler(kLastExceptionHandler, vectorExceptionHandling);
#endif
}

} // end namespace g2