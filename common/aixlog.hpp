/***
      __   __  _  _  __     __    ___
     / _\ (  )( \/ )(  )   /  \  / __)
    /    \ )(  )  ( / (_/\(  O )( (_ \
    \_/\_/(__)(_/\_)\____/ \__/  \___/
    version 1.5.3
    https://github.com/badaix/aixlog

    This file is part of aixlog
    Copyright (C) 2017-2025 Johannes Pohl

    This software may be modified and distributed under the terms
    of the MIT license.  See the LICENSE file for details.
***/

#ifndef AIX_LOG_HPP
#define AIX_LOG_HPP

#ifndef _WIN32
#define HAS_SYSLOG_ 1
#endif

#ifdef __APPLE__
#ifdef __MAC_OS_X_VERSION_MAX_ALLOWED
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 1012
#define HAS_APPLE_UNIFIED_LOG_ 1
#endif
#endif
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#pragma push_macro("ERROR")
#pragma push_macro("DEBUG")
#undef ERROR
#undef DEBUG
#endif

#ifdef HAS_APPLE_UNIFIED_LOG_
#include <os/log.h>
#endif

#ifdef HAS_SYSLOG_
#include <syslog.h>
#endif

#ifdef __ANDROID__
#ifdef __GNUC__
#define AIXLOG_INTERNAL__FUNC __FUNCTION__
#else
#define AIXLOG_INTERNAL__FUNC __func__
#endif
#else
#define AIXLOG_INTERNAL__FUNC __func__
#endif

/// Internal helper macros
#define AIXLOG_INTERNAL__LOG_SEVERITY(SEVERITY_) std::clog << static_cast<AixLog::Severity>(SEVERITY_) << AixLog::Tag()
#define AIXLOG_INTERNAL__LOG_SEVERITY_TAG(SEVERITY_, TAG_) std::clog << static_cast<AixLog::Severity>(SEVERITY_) << AixLog::Tag(TAG_)

#define AIXLOG_INTERNAL__ONE_COLOR(FG_) AixLog::Color::FG_
#define AIXLOG_INTERNAL__TWO_COLOR(FG_, BG_) AixLog::TextColor(AixLog::Color::FG_, AixLog::Color::BG_)

#define AIXLOG_INTERNAL__VAR_PARM(PARAM1_, PARAM2_, FUNC_, ...) FUNC_
#define AIXLOG_INTERNAL__LOG_MACRO_CHOOSER(...) AIXLOG_INTERNAL__VAR_PARM(__VA_ARGS__, AIXLOG_INTERNAL__LOG_SEVERITY_TAG, AIXLOG_INTERNAL__LOG_SEVERITY, )
#define AIXLOG_INTERNAL__COLOR_MACRO_CHOOSER(...) AIXLOG_INTERNAL__VAR_PARM(__VA_ARGS__, AIXLOG_INTERNAL__TWO_COLOR, AIXLOG_INTERNAL__ONE_COLOR, )

/// External logger macros
#define LOG(...) \
    (AixLog::Log::should_log_cached(__VA_ARGS__) ? \
        (AIXLOG_INTERNAL__LOG_MACRO_CHOOSER(__VA_ARGS__)(__VA_ARGS__) << AixLog::Timestamp(std::chrono::system_clock::now()) << AixLog::Function(AIXLOG_INTERNAL__FUNC, __FILE__, __LINE__)) : \
        AixLog::get_null_stream())

#define COLOR(...) AIXLOG_INTERNAL__COLOR_MACRO_CHOOSER(__VA_ARGS__)(__VA_ARGS__)

#define FUNC AixLog::Function(AIXLOG_INTERNAL__FUNC, __FILE__, __LINE__)
#define TAG AixLog::Tag
#define COND AixLog::Conditional
#define TIMESTAMP AixLog::Timestamp(std::chrono::system_clock::now())

enum SEVERITY
{
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    NOTICE = 3,
    WARNING = 4,
    ERROR = 5,
    FATAL = 6
};

namespace AixLog
{

enum class Severity : std::int8_t
{
    trace = SEVERITY::TRACE,
    debug = SEVERITY::DEBUG,
    info = SEVERITY::INFO,
    notice = SEVERITY::NOTICE,
    warning = SEVERITY::WARNING,
    error = SEVERITY::ERROR,
    fatal = SEVERITY::FATAL
};

static Severity to_severity(std::string severity, Severity def = Severity::info)
{
    std::transform(severity.begin(), severity.end(), severity.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (severity == "trace") return Severity::trace;
    else if (severity == "debug") return Severity::debug;
    else if (severity == "info") return Severity::info;
    else if (severity == "notice") return Severity::notice;
    else if (severity == "warning") return Severity::warning;
    else if (severity == "error") return Severity::error;
    else if (severity == "fatal") return Severity::fatal;
    else return def;
}

static std::string to_string(Severity logSeverity)
{
    switch (logSeverity)
    {
        case Severity::trace: return "Trace";
        case Severity::debug: return "Debug";
        case Severity::info: return "Info";
        case Severity::notice: return "Notice";
        case Severity::warning: return "Warn";
        case Severity::error: return "Error";
        case Severity::fatal: return "Fatal";
        default:
            std::stringstream ss;
            ss << static_cast<int>(logSeverity);
            return ss.str();
    }
}

enum class Color
{
    none = 0,
    black = 1,
    red = 2,
    green = 3,
    yellow = 4,
    blue = 5,
    magenta = 6,
    cyan = 7,
    white = 8
};

struct TextColor
{
    TextColor(Color foreground = Color::none, Color background = Color::none) : foreground(foreground), background(background) {}
    Color foreground;
    Color background;
};

struct Conditional
{
    using EvalFunc = std::function<bool()>;

    Conditional() : func_([](void) { return true; }) {}
    Conditional(EvalFunc func) : func_(std::move(func)) {}
    Conditional(bool value) : func_([value](void) { return value; }) {}
    virtual ~Conditional() = default;

    virtual bool is_true() const { return func_(); }

protected:
    EvalFunc func_;
};

struct Timestamp
{
    using time_point_sys_clock = std::chrono::time_point<std::chrono::system_clock>;

    Timestamp(std::nullptr_t) : is_null_(true) {}
    Timestamp() : Timestamp(nullptr) {}
    Timestamp(const time_point_sys_clock& time_point) : time_point(time_point), is_null_(false) {}
    Timestamp(time_point_sys_clock&& time_point) : time_point(time_point), is_null_(false) {}
    virtual ~Timestamp() = default;

    explicit operator bool() const { return !is_null_; }

    std::string to_string(const std::string& format = "%Y-%m-%d %H-%M-%S.#ms") const
    {
        std::time_t now_c = std::chrono::system_clock::to_time_t(time_point);
        struct ::tm now_tm = localtime_xp(now_c);
        char buffer[256];
        strftime(buffer, sizeof buffer, format.c_str(), &now_tm);
        std::string result(buffer);
        size_t pos = result.find("#ms");
        if (pos != std::string::npos)
        {
            int ms_part = std::chrono::time_point_cast<std::chrono::milliseconds>(time_point).time_since_epoch().count() % 1000;
            char ms_str[4];
            if (snprintf(ms_str, 4, "%03d", ms_part) >= 0)
                result.replace(pos, 3, ms_str);
        }
        return result;
    }

    time_point_sys_clock time_point;

private:
    bool is_null_;
    inline std::tm localtime_xp(std::time_t timer) const
    {
        std::tm bt;
#if defined(__unix__)
        localtime_r(&timer, &bt);
#elif defined(_MSC_VER)
        localtime_s(&bt, &timer);
#else
        static std::mutex mtx;
        std::lock_guard<std::mutex> lock(mtx);
        bt = *std::localtime(&timer);
#endif
        return bt;
    }
};

struct Tag
{
    Tag(std::nullptr_t) : is_null_(true) {}
    Tag() : Tag(nullptr) {}
    Tag(const char* text) : text(text ? text : ""), is_null_(text == nullptr) {}
    Tag(const std::string& text) : text(text), is_null_(false) {}
    Tag(std::string&& text) : text(std::move(text)), is_null_(false) {}
    virtual ~Tag() = default;

    explicit operator bool() const { return !is_null_; }
    bool operator<(const Tag& other) const { return (text < other.text); }

    std::string text;

private:
    bool is_null_;
};

struct Function
{
    Function(const std::string& name, const std::string& file, size_t line) : name(name), file(file), line(line), is_null_(false) {}
    Function(std::string&& name, std::string&& file, size_t line) : name(std::move(name)), file(std::move(file)), line(line), is_null_(false) {}
    Function(std::nullptr_t) : line(0), is_null_(true) {}
    Function() : Function(nullptr) {}
    virtual ~Function() = default;

    explicit operator bool() const { return !is_null_; }

    std::string name;
    std::string file;
    size_t line;

private:
    bool is_null_;
};

struct Metadata
{
    Metadata() : severity(Severity::trace), tag(nullptr), function(nullptr), timestamp(nullptr) {}
    Severity severity;
    Tag tag;
    Function function;
    Timestamp timestamp;
};

class Filter
{
public:
    Filter() {}
    Filter(Severity severity) { add_filter(severity); }

    bool match(const Metadata& metadata) const
    {
        if (tag_filter_.empty()) return true;
        auto iter = tag_filter_.find(metadata.tag);
        if (iter != tag_filter_.end()) return (metadata.severity >= iter->second);
        iter = tag_filter_.find("*");
        if (iter != tag_filter_.end()) return (metadata.severity >= iter->second);
        return false;
    }

    void add_filter(const Tag& tag, Severity severity) { tag_filter_[tag] = severity; }
    void add_filter(Severity severity) { tag_filter_["*"] = severity; }
    void add_filter(const std::string& filter)
    {
        auto pos = filter.find(':');
        if (pos != std::string::npos)
            add_filter(filter.substr(0, pos), to_severity(filter.substr(pos + 1)));
        else
            add_filter(to_severity(filter));
    }

private:
    std::map<Tag, Severity> tag_filter_;
};

struct Sink
{
    Sink(Filter filter) : filter(std::move(filter)) {}
    virtual ~Sink() = default;
    virtual void log(const Metadata& metadata, const std::string& message) = 0;

    Filter filter;
};

static std::ostream& operator<<(std::ostream& os, const Severity& log_severity);
static std::ostream& operator<<(std::ostream& os, const Timestamp& timestamp);
static std::ostream& operator<<(std::ostream& os, const Tag& tag);
static std::ostream& operator<<(std::ostream& os, const Function& function);
static std::ostream& operator<<(std::ostream& os, const Conditional& conditional);
static std::ostream& operator<<(std::ostream& os, const Color& color);
static std::ostream& operator<<(std::ostream& os, const TextColor& text_color);

using log_sink_ptr = std::shared_ptr<Sink>;

class Log : public std::basic_streambuf<char, std::char_traits<char>>
{
public:
    static Log& instance()
    {
        static Log instance_;
        return instance_;
    }

    static bool should_log(SEVERITY severity, const char* tag = nullptr)
    {
        return instance().should_log_internal(static_cast<Severity>(severity), tag);
    }

    static bool should_log(Severity severity, const char* tag = nullptr)
    {
        return instance().should_log_internal(severity, tag);
    }

    static bool should_log_cached(SEVERITY severity, const char* tag = nullptr);
    static bool should_log_cached(Severity severity, const char* tag = nullptr);
    static bool should_log_cached(Severity severity, const std::string& tag);

    static void clearShouldLogCache();
    static void getShouldLogCacheStats(size_t& hits, size_t& misses, size_t& size);
    static void setShouldLogCacheMaxSize(size_t size);

    static void init(const std::vector<log_sink_ptr>& log_sinks = {})
    {
        Log& log = instance();
        std::lock_guard<std::recursive_mutex> lock(log.mutex_);
        log.log_sinks_.clear();
        clearShouldLogCache();
        for (const auto& sink : log_sinks)
            log.add_logsink(sink);
    }

    template <typename T, typename... Ts>
    static std::shared_ptr<T> init(Ts&&... params)
    {
        std::shared_ptr<T> sink = instance().add_logsink<T>(std::forward<Ts>(params)...);
        init({sink});
        return sink;
    }

    template <typename T, typename... Ts>
    std::shared_ptr<T> add_logsink(Ts&&... params)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        static_assert(std::is_base_of<Sink, typename std::decay<T>::type>::value, "type T must be a Sink");
        std::shared_ptr<T> sink = std::make_shared<T>(std::forward<Ts>(params)...);
        log_sinks_.push_back(sink);
        return sink;
    }

    void add_logsink(const log_sink_ptr& sink)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        log_sinks_.push_back(sink);
        clearShouldLogCache();
    }

    void remove_logsink(const log_sink_ptr& sink)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        log_sinks_.erase(std::remove(log_sinks_.begin(), log_sinks_.end(), sink), log_sinks_.end());
        clearShouldLogCache();
    }

protected:
    Log() noexcept : last_buffer_(nullptr), do_log_(true)
    {
        std::clog.rdbuf(this);
        std::clog << Severity() << Tag() << Function() << Conditional() << Color::none << std::flush;
    }

    virtual ~Log()
    {
        sync();
    }

    int sync() override
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!get_stream().str().empty())
        {
            if (do_log_)
            {
                for (const auto& sink : log_sinks_)
                {
                    if (sink->filter.match(metadata_))
                        sink->log(metadata_, get_stream().str());
                }
            }
            get_stream().str("");
            get_stream().clear();
        }
        return 0;
    }

    int overflow(int c) override
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (c != EOF)
        {
            if (c == '\n')
                sync();
            else if (do_log_)
                get_stream() << static_cast<char>(c);
        }
        else
        {
            sync();
        }
        return c;
    }

private:
    friend std::ostream& operator<<(std::ostream& os, const Severity& log_severity);
    friend std::ostream& operator<<(std::ostream& os, const Timestamp& timestamp);
    friend std::ostream& operator<<(std::ostream& os, const Tag& tag);
    friend std::ostream& operator<<(std::ostream& os, const Function& function);
    friend std::ostream& operator<<(std::ostream& os, const Conditional& conditional);

    bool should_log_internal(Severity severity, const char* tag = nullptr)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (log_sinks_.empty()) return true;
        Metadata temp_metadata;
        temp_metadata.severity = severity;
        temp_metadata.tag = tag;
        for (const auto& sink : log_sinks_)
        {
            if (sink->filter.match(temp_metadata))
                return true;
        }
        return false;
    }

    std::stringstream& get_stream()
    {
        auto id = std::this_thread::get_id();
        if ((last_buffer_ == nullptr) || (last_id_ != id))
        {
            last_id_ = id;
            last_buffer_ = &(buffer_[id]);
        }
        return *last_buffer_;
    }

    std::map<std::thread::id, std::stringstream> buffer_;
    std::thread::id last_id_;
    std::stringstream* last_buffer_;
    Metadata metadata_;
    bool do_log_;
    std::vector<log_sink_ptr> log_sinks_;
    std::recursive_mutex mutex_;
};

struct SinkNull : public Sink
{
    SinkNull() : Sink(Filter()) {}
    void log(const Metadata& /*metadata*/, const std::string& /*message*/) override {}
};

struct SinkFormat : public Sink
{
    SinkFormat(const Filter& filter, const std::string& format) : Sink(filter), format_(format) {}

    virtual void set_format(const std::string& format) { format_ = format; }
    void log(const Metadata& metadata, const std::string& message) override = 0;

protected:
    virtual void do_log(std::ostream& stream, const Metadata& metadata, const std::string& message) const
    {
        std::string result = format_;
        if (metadata.timestamp)
            result = metadata.timestamp.to_string(result);

        size_t pos = result.find("#severity");
        if (pos != std::string::npos)
            result.replace(pos, 9, to_string(metadata.severity));

        pos = result.find("#color_severity");
        if (pos != std::string::npos)
        {
            std::stringstream ss;
            ss << TextColor(Color::red) << to_string(metadata.severity) << TextColor(Color::none);
            result.replace(pos, 15, ss.str());
        }

        pos = result.find("#tag_func");
        if (pos != std::string::npos)
            result.replace(pos, 9, metadata.tag ? metadata.tag.text : (metadata.function ? metadata.function.name : "log"));

        pos = result.find("#tag");
        if (pos != std::string::npos)
            result.replace(pos, 4, metadata.tag ? metadata.tag.text : "");

        pos = result.find("#function");
        if (pos != std::string::npos)
            result.replace(pos, 9, metadata.function ? metadata.function.name : "");

        pos = result.find("#message");
        if (pos != std::string::npos)
        {
            result.replace(pos, 8, message);
            stream << result << std::endl;
        }
        else
        {
            if (result.empty() || (result.back() == ' '))
                stream << result << message << std::endl;
            else
                stream << result << " " << message << std::endl;
        }
    }

    std::string format_;
};

struct SinkCout : public SinkFormat
{
    SinkCout(const Filter& filter, const std::string& format = "%Y-%m-%d %H-%M-%S.#ms [#severity] (#tag_func)") : SinkFormat(filter, format) {}
    void log(const Metadata& metadata, const std::string& message) override { do_log(std::cout, metadata, message); }
};

struct SinkCerr : public SinkFormat
{
    SinkCerr(const Filter& filter, const std::string& format = "%Y-%m-%d %H-%M-%S.#ms [#severity] (#tag_func)") : SinkFormat(filter, format) {}
    void log(const Metadata& metadata, const std::string& message) override { do_log(std::cerr, metadata, message); }
};

struct SinkFile : public SinkFormat
{
    SinkFile(const Filter& filter, const std::string& file_name, const std::string& format = "%Y-%m-%d %H-%M-%S.#ms [#severity] (#tag_func)")
        : SinkFormat(filter, format), file_(file_name, std::ios_base::app) {}
    void log(const Metadata& metadata, const std::string& message) override { do_log(file_, metadata, message); }

private:
    std::ofstream file_;
};

#ifdef HAS_SYSLOG_
struct SinkSyslog : public Sink
{
    SinkSyslog(const std::string& ident, const Filter& filter) : Sink(filter) { openlog(ident.c_str(), LOG_PID, LOG_USER); }
    ~SinkSyslog() override { closelog(); }
    void log(const Metadata& metadata, const std::string& message) override { syslog(static_cast<int>(metadata.severity), "%s", message.c_str()); }
};
#endif

#ifdef HAS_APPLE_UNIFIED_LOG_
struct SinkAppleSystemLog : public Sink
{
    SinkAppleSystemLog(const std::string& ident, const Filter& filter) : Sink(filter), ident_(ident) {}
    void log(const Metadata& metadata, const std::string& message) override
    {
        os_log_type_t type;
        switch (metadata.severity)
        {
            case Severity::trace:
            case Severity::debug: type = OS_LOG_TYPE_DEBUG; break;
            case Severity::info:
            case Severity::notice: type = OS_LOG_TYPE_INFO; break;
            case Severity::warning: type = OS_LOG_TYPE_DEFAULT; break;
            case Severity::error:
            case Severity::fatal: type = OS_LOG_TYPE_ERROR; break;
            default: type = OS_LOG_TYPE_DEFAULT; break;
        }
        os_log_with_type(os_log_create(ident_.c_str(), to_string(metadata.severity).c_str()), type, "%{public}s", message.c_str());
    }

private:
    std::string ident_;
};
#endif

#ifdef __ANDROID__
struct SinkLogcat : public Sink
{
    SinkLogcat(const std::string& ident, const Filter& filter) : Sink(filter), ident_(ident) {}
    void log(const Metadata& metadata, const std::string& message) override
    {
        android_LogPriority priority;
        switch (metadata.severity)
        {
            case Severity::trace: priority = ANDROID_LOG_VERBOSE; break;
            case Severity::debug: priority = ANDROID_LOG_DEBUG; break;
            case Severity::info:
            case Severity::notice: priority = ANDROID_LOG_INFO; break;
            case Severity::warning: priority = ANDROID_LOG_WARN; break;
            case Severity::error: priority = ANDROID_LOG_ERROR; break;
            case Severity::fatal: priority = ANDROID_LOG_FATAL; break;
            default: priority = ANDROID_LOG_DEFAULT; break;
        }
        __android_log_write(priority, ident_.c_str(), message.c_str());
    }

private:
    std::string ident_;
};
#endif

#ifdef _WIN32
struct SinkEventLog : public Sink
{
    SinkEventLog(const std::string& source, const Filter& filter) : Sink(filter), source_(source) { handle_ = RegisterEventSourceA(nullptr, source.c_str()); }
    ~SinkEventLog() override { if (handle_) DeregisterEventSource(handle_); }
    void log(const Metadata& metadata, const std::string& message) override
    {
        if (!handle_) return;
        WORD type;
        switch (metadata.severity)
        {
            case Severity::trace:
            case Severity::debug:
            case Severity::info:
            case Severity::notice: type = EVENTLOG_INFORMATION_TYPE; break;
            case Severity::warning: type = EVENTLOG_WARNING_TYPE; break;
            case Severity::error:
            case Severity::fatal: type = EVENTLOG_ERROR_TYPE; break;
            default: type = EVENTLOG_INFORMATION_TYPE; break;
        }
        const char* message_cstr = message.c_str();
        ReportEventA(handle_, type, 0, 0, nullptr, 1, 0, &message_cstr, nullptr);
    }

private:
    std::string source_;
    HANDLE handle_;
};
#endif

struct SinkCallback : public Sink
{
    using Callback = std::function<void(const Metadata& metadata, const std::string& message)>;
    SinkCallback(const Filter& filter, Callback callback) : Sink(filter), callback_(std::move(callback)) {}
    void log(const Metadata& metadata, const std::string& message) override { callback_(metadata, message); }

private:
    Callback callback_;
};

class NullBuffer : public std::streambuf
{
public:
    int overflow(int c) override { return c; }
};

class NullStream : public std::ostream
{
public:
    NullStream() : std::ostream(&buffer) {}
private:
    NullBuffer buffer;
};

inline NullStream& get_null_stream()
{
    static NullStream instance;
    return instance;
}

static std::ostream& operator<<(std::ostream& os, const Severity& log_severity)
{
    Log::instance().metadata_.severity = log_severity;
    return os;
}

static std::ostream& operator<<(std::ostream& os, const Timestamp& timestamp)
{
    Log::instance().metadata_.timestamp = timestamp;
    return os;
}

static std::ostream& operator<<(std::ostream& os, const Tag& tag)
{
    Log::instance().metadata_.tag = tag;
    return os;
}

static std::ostream& operator<<(std::ostream& os, const Function& function)
{
    Log::instance().metadata_.function = function;
    return os;
}

static std::ostream& operator<<(std::ostream& os, const Conditional& conditional)
{
    Log::instance().do_log_ = conditional.is_true();
    return os;
}

static std::ostream& operator<<(std::ostream& os, const Color& color)
{
    return os << TextColor(color);
}

static std::ostream& operator<<(std::ostream& os, const TextColor& text_color)
{
#ifdef _WIN32
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hConsole, &csbi);
    WORD current = csbi.wAttributes;
    WORD fg = 0;
    switch (text_color.foreground)
    {
        case Color::white: fg = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
        case Color::red: fg = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
        case Color::green: fg = FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
        case Color::blue: fg = FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
        case Color::yellow: fg = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
        case Color::cyan: fg = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
        case Color::magenta: fg = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
        case Color::black: fg = 0; break;
        case Color::none: fg = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; break;
    }
    WORD bg = 0;
    switch (text_color.background)
    {
        case Color::white: bg = BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY; break;
        case Color::red: bg = BACKGROUND_RED | BACKGROUND_INTENSITY; break;
        case Color::green: bg = BACKGROUND_GREEN | BACKGROUND_INTENSITY; break;
        case Color::blue: bg = BACKGROUND_BLUE | BACKGROUND_INTENSITY; break;
        case Color::yellow: bg = BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_INTENSITY; break;
        case Color::cyan: bg = BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY; break;
        case Color::magenta: bg = BACKGROUND_RED | BACKGROUND_BLUE | BACKGROUND_INTENSITY; break;
        case Color::black: bg = 0; break;
        case Color::none: bg = 0; break;
    }
    SetConsoleTextAttribute(hConsole, fg | bg);
#else
    const char* color_codes[] = {"", "30", "31", "32", "33", "34", "35", "36", "37"};
    const char* bg_color_codes[] = {"", "40", "41", "42", "43", "44", "45", "46", "47"};
    if (text_color.foreground == Color::none && text_color.background == Color::none)
        os << "\033[0m";
    else
        os << "\033[" << color_codes[static_cast<int>(text_color.foreground)] << ";" << bg_color_codes[static_cast<int>(text_color.background)] << "m";
#endif
    return os;
}

} // namespace AixLog

#ifdef _WIN32
#pragma pop_macro("ERROR")
#pragma pop_macro("DEBUG")
#endif

#endif // AIX_LOG_HPP

