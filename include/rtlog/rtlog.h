#pragma once

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>

#ifdef RTLOG_USE_FMTLIB
#include <fmt/format.h>
#endif // RTLOG_USE_FMTLIB

#include <readerwriterqueue.h>

#ifndef STB_SPRINTF_IMPLEMENTATION
#define STB_SPRINTF_IMPLEMENTATION
#endif

#ifndef STB_SPRINTF_STATIC
#define STB_SPRINTF_STATIC
#endif

#include <stb_sprintf.h>

namespace rtlog {

enum class Status {
  Success = 0,

  Error_QueueFull = 1,
  Error_MessageTruncated = 2,
};

/**
 * @brief A logger class for logging messages.
 * This class allows you to log messages of type LogData.
 * This type is user defined, and is often the additional data outside the
 * format string you want to log. For instance: The log level, the log region,
 * the file name, the line number, etc. See examples or tests for some ideas.
 *
 * TODO: Currently is built on a single input/single output queue. Do not call
 * Log or PrintAndClearLogQueue from multiple threads.
 *
 * @tparam LogData The type of the data to be logged.
 * @tparam MaxNumMessages The maximum number of messages that can be enqueud at
 * once. If this number is exceeded, the logger will return an error.
 * @tparam MaxMessageLength The maximum length of each message. Messages longer
 * than this will be truncated and still enqueued
 * @tparam SequenceNumber This number is incremented when the message is
 * enqueued. It is assumed that your non-realtime logger increments and logs it
 * on Log.
 */
template <typename LogData, size_t MaxNumMessages, size_t MaxMessageLength,
          std::atomic<std::size_t> &SequenceNumber>
class Logger {
public:
  /*
   * @brief Logs a message with the given format and input data.
   *
   * REALTIME SAFE; you are supposed to allocate va_list in realtime safe
   * manner, or expect that the system does not allocate va_args.
   *
   * This function logs a message with the given format and input data. The
   * format is specified using printf-style format specifiers. It's highly
   * recommended you use and respect -Wformat to ensure your format specifiers
   * are correct.
   *
   * To actually process the log messages (print, write to file, etc) you must
   * call PrintAndClearLogQueue.
   *
   * @param inputData The data to be logged.
   * @param format The printf-style format specifiers for the message.
   * @param args The variable arguments to the printf-style format specifiers.
   * @return Status A Status value indicating whether the logging operation was
   * successful.
   *
   * This function attempts to enqueue the log message regardless of whether the
   * message was truncated due to being too long for the buffer. If the message
   * queue is full, the function returns `Status::Error_QueueFull`. If the
   * message was truncated, the function returns
   * `Status::Error_MessageTruncated`. Otherwise, it returns `Status::Success`.
   */
  Status Logv(LogData &&inputData, const char *format, va_list args) {
    auto retVal = Status::Success;

    InternalLogData dataToQueue;
    dataToQueue.mLogData = std::forward<LogData>(inputData);
    dataToQueue.mSequenceNumber =
        SequenceNumber.fetch_add(1, std::memory_order_relaxed);

    const auto charsPrinted = stbsp_vsnprintf(
        dataToQueue.mMessage.data(), dataToQueue.mMessage.size(), format, args);

    if (charsPrinted < 0 ||
        static_cast<size_t>(charsPrinted) >= dataToQueue.mMessage.size())
      retVal = Status::Error_MessageTruncated;

    // Even if the message was truncated, we still try to enqueue it to minimize
    // data loss
    const bool dataWasEnqueued = mQueue.try_enqueue(dataToQueue);

    if (!dataWasEnqueued)
      retVal = Status::Error_QueueFull;

    return retVal;
  }

  /*
   * @brief Logs a message with the given format and input data.
   *
   * REALTIME SAFE - except on systems where va_args allocates
   *
   * This function logs a message with the given format and input data. The
   * format is specified using printf-style format specifiers. It's highly
   * recommended you use and respect -Wformat to ensure your format specifiers
   * are correct.
   *
   * To actually process the log messages (print, write to file, etc) you must
   * call PrintAndClearLogQueue.
   *
   * @param inputData The data to be logged.
   * @param format The printf-style format specifiers for the message.
   * @param ... The variable arguments to the printf-style format specifiers.
   * @return Status A Status value indicating whether the logging operation was
   * successful.
   *
   * This function attempts to enqueue the log message regardless of whether the
   * message was truncated due to being too long for the buffer. If the message
   * queue is full, the function returns `Status::Error_QueueFull`. If the
   * message was truncated, the function returns
   * `Status::Error_MessageTruncated`. Otherwise, it returns `Status::Success`.
   */
  Status Log(LogData &&inputData, const char *format, ...)
#ifndef __MSC_VER__
      __attribute__((format(printf, 3, 4)))
#endif
  {
    va_list args;
    va_start(args, format);
    auto retVal = Logv(std::move(inputData), format, args);
    va_end(args);
    return retVal;
  }

#ifdef RTLOG_USE_FMTLIB

  /**
   * @brief Logs a message with the given format string and input data.
   *
   * REALTIME SAFE ON ALL SYSTEMS!
   *
   * This function logs a message using a format string and input data, similar
   * to the `Log` function. However, instead of printf-style format specifiers,
   * this function uses the format specifiers of the {fmt} library. Because the
   * variadic template is resolved at compile time, this is guaranteed to be
   * realtime safe on all systems.
   *
   * To actually process the log messages (print, write to file, etc), you must
   * call PrintAndClearLogQueue.
   *
   * @tparam T The types of the arguments to the format specifiers.
   * @param inputData The data to be logged.
   * @param fmtString The {fmt}-style format string for the message.
   * @param args The arguments to the format specifiers.
   * @return Status A Status value indicating whether the logging operation was
   * successful.
   *
   * This function attempts to enqueue the log message regardless of whether the
   * message was truncated due to being too long for the buffer. If the message
   * queue is full, the function returns `Status::Error_QueueFull`. If the
   * message was truncated, the function returns
   * `Status::Error_MessageTruncated`. Otherwise, it returns `Status::Success`.
   */
  template <typename... T>
  Status LogFmt(LogData &&inputData, fmt::format_string<T...> fmtString,
                T &&...args) {
    auto retVal = Status::Success;

    InternalLogData dataToQueue;
    dataToQueue.mLogData = std::forward<LogData>(inputData);
    dataToQueue.mSequenceNumber =
        SequenceNumber.fetch_add(1, std::memory_order_relaxed);

    const auto maxMessageLength =
        dataToQueue.mMessage.size() - 1; // Account for null terminator

    const auto result =
        fmt::format_to_n(dataToQueue.mMessage.data(), maxMessageLength,
                         fmtString, std::forward<T>(args)...);

    if (result.size >= dataToQueue.mMessage.size()) {
      dataToQueue.mMessage[dataToQueue.mMessage.size() - 1] = '\0';
      retVal = Status::Error_MessageTruncated;
    } else
      dataToQueue.mMessage[result.size] = '\0';

    // Even if the message was truncated, we still try to enqueue it to minimize
    // data loss
    const bool dataWasEnqueued = mQueue.try_enqueue(dataToQueue);

    if (!dataWasEnqueued)
      retVal = Status::Error_QueueFull;

    return retVal;
  };

#endif // RTLOG_USE_FMTLIB

  /**
   * @brief Processes and prints all queued log data.
   *
   * ONLY REALTIME SAFE IF printLogFn IS REALTIME SAFE! - not generally the case
   *
   * This function processes and prints all queued log data. It takes a
   * PrintLogFn object as input, which is used to print the log data.
   *
   * See tests and examples for some ideas on how to use this function. Using
   * ctad you often don't need to specify the template parameter.
   *
   * @tparam PrintLogFn The type of the print log function object.
   * @param printLogFn The print log function object to be used to print the log
   * data.
   * @return int The number of log messages that were processed and printed.
   */
  template <typename PrintLogFn>
  int PrintAndClearLogQueue(PrintLogFn &printLogFn) {
    int numProcessed = 0;

    InternalLogData value;
    while (mQueue.try_dequeue(value)) {
      printLogFn(value.mLogData, value.mSequenceNumber, "%s",
                 value.mMessage.data());
      numProcessed++;
    }

    return numProcessed;
  }

private:
  struct InternalLogData {
    LogData mLogData{};
    size_t mSequenceNumber{};
    std::array<char, MaxMessageLength> mMessage{};
  };

  moodycamel::ReaderWriterQueue<InternalLogData> mQueue{MaxNumMessages};
};

/**
 * @brief A class representing a log processing thread.
 *
 * This class represents a log processing thread that continuously dequeues log
 * data from a LoggerType object and calls a PrintLogFn object to print the log
 * data. The wait time between each log processing iteration can be specified in
 * milliseconds.
 *
 * @tparam LoggerType The type of the logger object to be used for log
 * processing.
 * @tparam PrintLogFn The type of the print log function object.
 */
template <typename LoggerType, typename PrintLogFn> class LogProcessingThread {
public:
  /**
   * @brief Constructs a new LogProcessingThread object.
   *
   * This constructor creates a new LogProcessingThread object. It takes a
   * reference to a LoggerType object, generally assumed to be some
   * specialization of rtlog::Logger, a reference to a PrintLogFn object, and a
   * wait time in ms
   *
   * On construction, the LogProcessingThread will start a thread that will
   * continually dequeue the messages from the logger and call printFn on them.
   *
   * You must call Stop() to stop the thread and join it before your logger goes
   * out of scope! Otherwise it's a use-after-free
   *
   * See tests and examples for some ideas on how to use this class. Using ctad
   * you often don't need to specify the template parameters.
   *
   * @param logger The logger object to be used for log processing.
   * @param printFn The print log function object to be used to print the log
   * data.
   * @param waitTime The time to wait between each log processing iteration.
   */
  LogProcessingThread(LoggerType &logger, PrintLogFn &printFn,
                      std::chrono::milliseconds waitTime)
      : mPrintFn(printFn), mLogger(logger), mWaitTime(waitTime) {
    mThread = std::thread(&LogProcessingThread::ThreadMain, this);
  }

  ~LogProcessingThread() {
    if (mThread.joinable()) {
      Stop();
      mThread.join();
    }
  }

  void Stop() { mShouldRun.store(false); }

  LogProcessingThread(const LogProcessingThread &) = delete;
  LogProcessingThread &operator=(const LogProcessingThread &) = delete;
  LogProcessingThread(LogProcessingThread &&) = delete;
  LogProcessingThread &operator=(LogProcessingThread &&) = delete;

private:
  void ThreadMain() {
    while (mShouldRun.load()) {

      if (mLogger.PrintAndClearLogQueue(mPrintFn) == 0)
        std::this_thread::sleep_for(mWaitTime);

      std::this_thread::sleep_for(mWaitTime);
    }

    mLogger.PrintAndClearLogQueue(mPrintFn);
  }

  PrintLogFn &mPrintFn{};
  LoggerType &mLogger{};
  std::thread mThread{};
  std::atomic<bool> mShouldRun{true};
  std::chrono::milliseconds mWaitTime{};
};

} // namespace rtlog
