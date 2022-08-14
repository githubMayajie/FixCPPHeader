#include <cstdint>
#include <string>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <initializer_list>
#include <list>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <assert.h>
#include <functional>
#include <string_view>
#include <regex>
#include <iostream>


#ifdef _MSC_VER
#include <Windows.h>
#define MY_PRINT_STR(buf) std::printf("%s\n",buf);OutputDebugStringA(buf);OutputDebugStringA("\n");
#else
#define MY_PRINT_STR(buf) std::printf("%s\n",buf);
#endif

static void showPluginLog(const char* format, ...)
{
    std::string contentStr = "";
    size_t size = 1024;
    char stackbuf[1024];
    std::vector<char> dynamicbuf;
    char* buf = &stackbuf[0];
    va_list args;
    while (true) {
        va_start(args, format);
        int needed = std::vsnprintf(buf, size, format, args);
        va_end(args);
        if (needed <= (int)size && needed >= 0) {
            contentStr.assign(buf, (size_t)needed);
            break;
        }
        size = (needed > 0) ? (needed + 1) : (size * 2);
        dynamicbuf.resize(size);
        buf = &dynamicbuf[0];
    }
    MY_PRINT_STR(contentStr.c_str());
}

#define MY_LOG_INFO(format,...) showPluginLog(format,##__VA_ARGS__)


namespace MyLog
{
    std::vector<std::string> logs;
    std::string recordLog(const char* format, ...)
    {
        std::string contentStr = "";
        size_t size = 1024;
        char stackbuf[1024];
        std::vector<char> dynamicbuf;
        char* buf = &stackbuf[0];
        va_list args;
        while (true) {
            va_start(args, format);
            int needed = std::vsnprintf(buf, size, format, args);
            va_end(args);
            if (needed <= (int)size && needed >= 0) {
                contentStr.assign(buf, (size_t)needed);
                break;
            }
            size = (needed > 0) ? (needed + 1) : (size * 2);
            dynamicbuf.resize(size);
            buf = &dynamicbuf[0];
        }
        return contentStr;
    }

    void recordLog(const std::string& log)
    {
        // only call in mainthread
        logs.emplace_back(log);
    }

    void printRecordLog()
    {
        // only call in mainthread
        for (auto& one : logs)
        {
            MY_PRINT_STR(one.c_str());
        }
    }
}


namespace MyThreadPool
{
    class AsyncTask;
    class TaskThread;
    class ThreadPool;

    class AsyncTask
    {
        friend class TaskThread;
    public:
        AsyncTask()
            :refCount(1),
            abort(false)
        {
        }

        virtual void setAbort(bool newStatus)
        {
            abort = newStatus;
        }
        bool isAbort()
        {
            return abort;
        }

        void retain()
        {
            ++refCount;
        }

        void release()
        {
            --refCount;
            if (refCount == 0)
            {
                delete this;
            }
        }
        int getRefCount()
        {
            return refCount;
        }

        virtual void run() {};
        virtual void onStart() {};
        virtual void onProgress() {};
        virtual void onEnd() {};
    protected:
        std::atomic<bool> abort = false;
        std::atomic<uint32_t> refCount = 1;
    };

    class TaskThread
    {
    public:
        TaskThread()
        {
            asyncRuntime = new std::thread([this]() {
                while (true)
                {
                    {
                        std::unique_lock<std::mutex> lk(critical_mutex);
                        critical_cv.wait(lk, [this] {
                            return status != 0;
                            });
                        lk.unlock();
                    }
                    {
                        std::lock_guard<std::mutex> lk(critical_mutex);
                        if (status == 2)
                        {
                            break;
                        }
                    }
                    currentTask->run();
                    {
                        std::lock_guard<std::mutex> lk(critical_mutex);
                        if (status == 2)
                        {
                            break;
                        }
                        status = 0;
                    }
                }
                delete this->asyncRuntime;
                this->asyncRuntime = nullptr;
                delete this;
                });
            asyncRuntime->detach();
        }
        ~TaskThread()
        {

        }
        std::thread* asyncRuntime = nullptr;
        std::mutex critical_mutex;
        std::condition_variable critical_cv;
        AsyncTask* currentTask = nullptr;

        // 0 waitTask 1 hasTask 2 quit
        int status = 0;
    };

    class ThreadPool
    {
    public:
        void tick()
        {
            std::vector<TaskThread*> waitTaskThreads;
            std::vector<TaskThread*> runTaskThreads;
            for (const auto& one : taskThreads)
            {
                int status = 0;
                {
                    std::lock_guard<std::mutex> lk(one->critical_mutex);
                    status = one->status;
                }
                if (status == 0)
                {
                    if (one->currentTask)
                    {
                        if (!one->currentTask->isAbort())
                        {
                            one->currentTask->onEnd();
                        }
                        one->currentTask->release();
                        one->currentTask = nullptr;
                    }
                    waitTaskThreads.push_back(one);
                }
                else if (status == 1)
                {
                    if (!one->currentTask->isAbort())
                    {
                        one->currentTask->onProgress();
                    }
                    runTaskThreads.push_back(one);
                }
            }
            //reuse wait taskThread
            auto reuseTaskThreadCount = 0;
            auto waitTaskThreadsCount = waitTaskThreads.size();
            auto taskCount = tasks.size();
            if (waitTaskThreadsCount > 0 && taskCount > 0)
            {
                for (int i = 0; i < (taskCount > waitTaskThreadsCount ? waitTaskThreadsCount : taskCount); i++)
                {
                    reuseTaskThreadCount++;
                    auto task = tasks.front();
                    tasks.pop_front();
                    auto& taskThread = waitTaskThreads[i];
                    taskThread->currentTask = task;
                    task->onStart();
                    {
                        std::lock_guard<std::mutex> lk(taskThread->critical_mutex);
                        taskThread->status = 1;
                        taskThread->critical_cv.notify_one();
                    }
                }
                taskCount = taskCount - reuseTaskThreadCount;
            }

            //add new taskThread
            auto leftTaskThreadCount = maxThreadCount - (reuseTaskThreadCount + runTaskThreads.size());
            auto newTaskThreadCount = 0;
            if (leftTaskThreadCount > 0 && taskCount > 0)
            {
                for (int i = 0; i < (taskCount > leftTaskThreadCount ? leftTaskThreadCount : taskCount); i++)
                {
                    newTaskThreadCount++;
                    auto task = tasks.front();
                    tasks.pop_front();
                    auto taskThread = new TaskThread;
                    taskThreads.push_back(taskThread);
                    taskThread->currentTask = task;
                    task->onStart();
                    {
                        std::lock_guard<std::mutex> lk(taskThread->critical_mutex);
                        taskThread->status = 1;
                        taskThread->critical_cv.notify_one();
                    }
                }
                taskCount = taskCount - newTaskThreadCount;
            }

            // remove useless taskThread
            if (taskCount <= 0)
            {
                for (auto iter = taskThreads.begin(); iter != taskThreads.end();)
                {
                    auto& taskThread = *iter;
                    auto needNotify = false;
                    {
                        std::lock_guard<std::mutex> lk(taskThread->critical_mutex);
                        if (taskThread->status == 0)
                        {
                            taskThread->status = 2;
                            needNotify = true;
                            taskThread->critical_cv.notify_one();
                        }
                    }
                    if (needNotify)
                    {
                        iter = taskThreads.erase(iter);
                    }
                    else
                    {
                        iter++;
                    }
                }
            }
        }
        void addTask(AsyncTask* p1)
        {
            tasks.push_back(p1);
        }
        void abortTask(AsyncTask* p1)
        {
            for (auto iter = tasks.begin(); iter != tasks.end();)
            {
                if (*iter == p1)
                {
                    iter = tasks.erase(iter);
                    p1->release();
                    return;
                }
                else
                {
                    iter++;
                }
            }

            for (const auto& one : taskThreads)
            {
                if (one->currentTask == p1)
                {
                    one->currentTask->setAbort(true);
                    return;
                }
            }
        }
        std::uint8_t maxThreadCount = 0;
        std::size_t taskSize() { return tasks.size() + taskThreads.size(); }
        void clear()
        {
            for (auto& one : tasks)
            {
                one->release();
            }
            tasks.clear();
            for (auto& one : taskThreads)
            {
                one->currentTask->setAbort(true);
                {
                    std::lock_guard<std::mutex> lk(one->critical_mutex);
                    one->status = 2;
                    one->critical_cv.notify_one();
                }
            }
            taskThreads.clear();
        }
    protected:
        std::list<AsyncTask*> tasks;
        std::vector<TaskThread*> taskThreads;
    };
}


// from https://en.cppreference.com/w/cpp/header
namespace languageCoreHeader
{
    static const std::initializer_list<std::tuple<std::string, std::uint8_t>> standardCPPHeaders =
    {
        // Concepts library
        {"concepts",20},

        // Coroutines library
        {"coroutine",20},
        {"generator",23},

        // Utilities library
        {"any",17},
        {"bitset",0},
        {"chrono",11},
        {"compare",20},
        {"csetjmp",0},
        {"csignal",0},
        {"cstdarg",0},
        {"cstddef",0},
        {"cstdlib",0},
        {"ctime",0},
        {"expected",23},
        {"functional",0},
        {"initializer_list",11},
        {"optional",17},
        {"source_location",20},
        {"tuple",11},
        {"type_traits",11},
        {"typeindex",11},
        {"typeinfo",0},
        {"utility",0},
        {"variant",17},
        {"version",20},

        // Dynamic memory management 
        {"memory",0},
        {"memory_resource",17},
        {"new",0},
        {"scoped_allocator",11},

        // Numeric limits 
        {"cfloat",0},
        {"cinttypes",11},
        {"climits",0},
        {"cstdint",11},
        {"limits",0},
        {"stdfloat",23},

        // Error handling 
        {"cassert",0},
        {"cerrno",0},
        {"exception",0},
        {"stacktrace",23},
        {"stdexcept",0},
        {"system_error",11},

        // Strings library
        {"cctype",0},
        {"charconv",17},
        {"cstring",0},
        {"cuchar",11},
        {"cwchar",0},
        {"cwctype",0},
        {"format",20},
        {"string",0},
        {"string_view",17},

        // Containers library
        {"array", 11},
        {"deque",0},
        {"flat_map",23},
        {"flat_set",23},
        {"forward_list",11},
        {"list",0},
        {"map",0},
        {"mdspan",23},
        {"queue",0},
        {"set",0},
        {"span",20},
        {"stack",0},
        {"unordered_map",11},
        {"unordered_set",11},
        {"vector",0},

        // Iterators library
        {"iterator",0},

        // Ranges library
        {"ranges",20},

        // Algorithms library
        {"algorithm",0},
        {"execution",17},

        // Numerics library
        {"bit",20},
        {"cfenv",11},
        {"cmath",0},
        {"complex",0},
        {"numbers",20},
        {"numeric",0},
        {"random",11},
        {"ratio",11},
        {"valarray",0},

        // Localization library
        {"clocale",0},
        {"codecvt",11},
        {"locale",0},

        // Input/output library
        {"cstdio",0},
        {"fstream",0},
        {"iomanip",0},
        {"ios",0},
        {"iosfwd",0},
        {"iostream",0},
        {"istream",0},
        {"ostream",0},
        {"print",23},
        {"spanstream",23},
        {"sstream",0},
        {"streambuf",0},
        {"strstream",0},
        {"syncstream",20},

        // Filesystem library
        {"filesystem",17},

        // Regular Expressions library
        {"regex",11},

        // Atomic Operations library
        {"atomic",11},

        // Thread support library
        {"barrier",20},
        {"condition_variable",11},
        {"future",11},
        {"latch",20},
        {"mutex",11},
        {"semaphore",20},
        {"shared_mutex",14},
        {"stop_token",20},
        {"thread",11},
    };

    // name deprecated_version remove_version
    static const std::initializer_list<std::tuple<std::string, std::uint8_t, std::uint8_t>> standardDeprecatedCPPHeaders =
    {
        {"codecvt",17,0xff},
        {"strstream",0,0xff},
    };

    static const std::initializer_list<std::tuple<std::string, std::uint8_t>> standardCHeaders =
    {
        {"assert.h",0},
        {"ctype.h",0},
        {"errno.h",0},
        {"fenv.h",11},
        {"float.h",0},
        {"inttypes.h",11},
        {"limits.h",0},
        {"locale.h",0},
        {"math.h",0},
        {"setjmp.h",0},
        {"signal.h",0},
        {"stdarg.h",0},
        {"stddef.h",0},
        {"stdint.h",11},
        {"stdio.h",0},
        {"stdlib.h",0},
        {"string.h",0},
        {"time.h",0},
        {"uchar.h",11},
        {"wchar.h",0},
        {"wctype.h",0},

        // Special C compatibility headers
        {"stdatomic.h",23},

        // Empty C headers
        {"ccomplex",11},
        {"complex.h",11},
        {"ctgmath",11},
        {"tgmath.h",11},

        // Meaningless C headers
        {"ciso646",0},
        {"cstdalign",11},
        {"cstdbool",11},
        {"iso646.h",0},
        {"stdalign.h",11},
        {"stdbool.h",11},
    };

    // name deprecated_version remove_version
    static const std::initializer_list<std::tuple<std::string, std::uint8_t, std::uint8_t>> standardDeprecatedCHeaders =
    {
        {"ccomplex",17,20},
        {"ctgmath",17,20},
        {"ciso646",0,20},
        {"cstdalign",17,20},
        {"cstdbool",17,20},
    };
}

#pragma warning(disable : 4996)

namespace FileSystem
{
    namespace fs = std::filesystem;
    // static bool if_path_exists(const fs::path& p, fs::file_status s = fs::file_status{})
    // {
    //     if(fs::status_known(s))
    //     {
    //         return fs::exists(s);
    //     }
    //     else
    //     {
    //         return fs::exists(p);
    //     }
    // }
    static std::string convertPath(const std::string& input)
    {
        auto inputPath = fs::path(input);
        auto curPath = fs::current_path();
        if (fs::exists(inputPath))
        {
            return inputPath.string();
        }
        auto inputPath2 = fs::current_path() / inputPath;
        if (fs::exists(inputPath2))
        {
            return inputPath2.string();
        }
        return "";
    }

    static int writeFile(const std::string& input, const std::vector<std::uint8_t>& content)
    {
        auto path = convertPath(input);
        if (path.empty())
        {
            return -1;
        }
        FILE* fp = fopen(path.c_str(), "wb+");
        if (!fp)
        {
            return -2;
        }
        auto num = fwrite(content.data(), 1, content.size(), fp);
        if (num == 0)
        {
            fclose(fp);
            return -3;
        }
        fclose(fp);
        return 0;
    }

    static bool readFile(const std::string& input, std::vector<std::uint8_t>& content)
    {
        auto path = convertPath(input);
        if (path.empty())
        {
            return -1;
        }
        FILE* fp = fopen(path.c_str(), "rb+");
        if (!fp)
        {
            return -2;
        }
        fseek(fp, 0, SEEK_END);
        size_t fileSize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        content.resize(fileSize, '\0');
        auto readCount = fread(content.data(), 1, fileSize, fp);
        if (readCount == 0)
        {
            fclose(fp);
            return -3;
        }
        fclose(fp);
        return 0;
    }

    static bool isDir(const std::string& input)
    {
        auto inputPath = fs::path(input);
        if (fs::exists(inputPath))
        {
            return fs::is_directory(inputPath);
        }
        return false;
    }

    static bool isFile(const std::string& input)
    {
        auto inputPath = fs::path(input);
        if (fs::exists(inputPath))
        {
            return fs::is_regular_file(inputPath);
        }
        return false;
    }

    static void getChildren(const std::string& path, std::vector<std::tuple<bool, std::string, std::string, std::string>>& ret)
    {
        // bool     string          string
        // isDir    relativePath    fullPath
        std::function<void(const fs::path&, const std::string&)> visitFunc;
        visitFunc = [&ret, &visitFunc](const fs::path& one, const std::string& relativePath) {
            for (const auto& entry : fs::directory_iterator(one))
            {
                auto isDir = entry.is_directory();
                auto tempPath = fs::absolute(entry.path());
                auto fileName = tempPath.filename().string();
                if (fileName == "." || fileName == "..")
                {
                    continue;
                }
                auto relativeName = relativePath + fileName;
                auto fullPath = tempPath.string();
                ret.emplace_back(isDir, fileName, relativeName, fullPath);
                if (isDir)
                {
                    visitFunc(tempPath, relativeName + "/");
                }
            }
        };
        if (isDir(path))
        {
            visitFunc(path, "");
        }
        else
        {
            if (isFile(path))
            {
                ret.emplace_back(false, fs::path(path).filename().string(), path, path);
            }
        }
    }

    // all need check file
    static std::vector<std::string> files;

    // all include file (key = fileName, fileRelativeIncludeRootPath)
    static std::unordered_map<std::string, std::string> includePaths;
}

namespace ConfigureParams
{

    static constexpr auto helpDocument = R"help(-h show help(this documentation)
-s standard([0,11,14,17,20,23]) (default C++23)
-i include include path[str(filepath/dirpath)]
-e exclude include file path[str(filename)]
-sk skip header file[str(filename)]
-ul use local if conflicit with standard file[str(filename)]
-if inlcude fix root path[str(default = exe run path)]
-ef exclude fix root path[str(filepath/dirpath)]
-mt max thread[number(default = 2 * std::thread::hardware_concurrency() + 1)]
-dp disable pause[true|false](default = false))
-fs add file suffix[str])help";

    static std::vector<std::string> inputParams;
    static int argc = 0;
    static char** argv = nullptr;

    static bool showHelp = false;
    static std::uint8_t languageVersion = 23;
    static std::vector<std::string> includePaths;
    static std::vector<std::string> excludePaths;
    static std::vector<std::string> skipFileNames;
    static std::vector<std::string> useLocalFileNames;
    static std::vector<std::string> includeFixPaths;
    static std::vector<std::string> excludeFixPaths;
    static std::uint32_t maxThread = 2 * std::thread::hardware_concurrency() + 1;
    static bool disablePause = false;
    static std::vector<std::string> fileSuffix = {
        "h","cpp","hpp","c","c++","mm"
    };

    static std::vector<std::uint8_t> supportLanguageVersion = {
        0,11,14,17,20,23
    };

    static void parseInputParams(int p1, char** p2)
    {
        argc = p1;
        argv = p2;
        inputParams.clear();
        for (int i = 0; i < p1; i++)
        {
            inputParams.emplace_back((const char*)(p2[i]));
        }

        bool parseError = false;
        for (int i = 0; i < p1;)
        {
            auto value = inputParams[i];
            auto nextValue = std::string("");
            bool hasNextValue = false;
            if (i + 1 < p1)
            {
                nextValue = inputParams[i + 1];
                hasNextValue = true;
            }
            if (value == "-h")
            {
                parseError = true;
                break;
            }
            else if (value == "-s")
            {
                parseError = true;
                if (hasNextValue)
                {
                    try
                    {
                        languageVersion = std::stoi(nextValue);
                        for (auto& one : supportLanguageVersion)
                        {
                            if (languageVersion == one)
                            {
                                parseError = false;
                                break;
                            }
                        }
                    }
                    catch (std::invalid_argument const& ex)
                    {
                    }
                    catch (std::out_of_range const& ex)
                    {
                    }
                }
                if (parseError)
                {
                    MY_PRINT_STR("-s [0,11,14,17,20,23] parse error");
                    break;
                }
                i += 2;
            }
            else if (value == "-i")
            {
                parseError = true;
                if (hasNextValue)
                {
                    std::string newPath = FileSystem::convertPath(nextValue);
                    if (!newPath.empty())
                    {
                        parseError = false;
                        includePaths.emplace_back(newPath);
                    }
                }
                if (parseError)
                {
                    MY_PRINT_STR("-i include include path[str(filepath/dirpath)] parse error");
                    break;
                }
                i += 2;
            }
            else if (value == "-e")
            {
                parseError = true;
                if (hasNextValue)
                {
                    std::string newPath = FileSystem::convertPath(nextValue);
                    if (!newPath.empty())
                    {
                        parseError = false;
                        excludePaths.emplace_back(newPath);
                    }
                }
                if (parseError)
                {
                    MY_PRINT_STR("-e exclude include file path[str(filename)] parse error");
                    break;
                }
                i += 2;
            }
            else if (value == "-sk")
            {
                parseError = true;
                if (hasNextValue)
                {
                    parseError = false;
                    skipFileNames.emplace_back(nextValue);
                }
                if (parseError)
                {
                    MY_PRINT_STR("-sk skip file[str(filename)] parse error");
                    break;
                }
                i += 2;
            }
            else if (value == "-ul")
            {
                parseError = true;
                if (hasNextValue)
                {
                    parseError = false;
                    useLocalFileNames.emplace_back(nextValue);
                }
                if (parseError)
                {
                    MY_PRINT_STR("-ul use local if conflicit with standard file[str(filename)] parse error");
                    break;
                }
                i += 2;
            }
            else if (value == "-if")
            {
                parseError = true;
                if (hasNextValue)
                {
                    std::string newPath = FileSystem::convertPath(nextValue);
                    if (!newPath.empty())
                    {
                        parseError = false;
                        includeFixPaths.emplace_back(newPath);
                    }
                }
                if (parseError)
                {
                    MY_PRINT_STR("-if inlcude fix root path[str(default = exe run path)] parse error");
                    break;
                }
                i += 2;
            }
            else if (value == "-ef")
            {
                parseError = true;
                if (hasNextValue)
                {
                    std::string newPath = FileSystem::convertPath(nextValue);
                    if (!newPath.empty())
                    {
                        parseError = false;
                        excludeFixPaths.emplace_back(newPath);
                    }
                }
                if (parseError)
                {
                    MY_PRINT_STR("-if inlcude fix root path[str(default = exe run path)] parse error");
                    break;
                }
                i += 2;
            }
            else if (value == "-mt")
            {
                parseError = true;
                if (hasNextValue)
                {
                    try
                    {
                        maxThread = std::stoi(nextValue);
                        if (maxThread > 0 && maxThread < 1000)
                        {
                            parseError = false;
                        }
                    }
                    catch (std::invalid_argument const& ex)
                    {
                    }
                    catch (std::out_of_range const& ex)
                    {
                    }
                }
                if (parseError)
                {
                    MY_PRINT_STR("-mt max thread[number(default = 2 * std::thread::hardware_concurrency() + 1)] parse error");
                    break;
                }
                i += 2;
            }
            else if (value == "-dp")
            {
                parseError = true;
                if (hasNextValue)
                {
                    if (nextValue == "true")
                    {
                        parseError = false;
                        disablePause = true;
                    }
                    else if (nextValue == "false")
                    {
                        parseError = false;
                        disablePause = false;
                    }
                }
                if (parseError)
                {
                    MY_PRINT_STR("-dp disable pause[true|false](default = false)) parse error");
                    break;
                }
                i += 2;
            }
            else if (value == "-fs")
            {
                parseError = true;
                if (hasNextValue)
                {
                    parseError = false;
                    bool isFind = false;
                    for (auto& one : fileSuffix)
                    {
                        if (one == nextValue)
                        {
                            isFind = true;
                            break;
                        }
                    }
                    fileSuffix.emplace_back(nextValue);
                }
                if (parseError)
                {
                    MY_PRINT_STR("-dp disable pause[true|false](default = false)) parse error");
                    break;
                }
                i += 2;
            }
            else if (i == 0)
            {
                ++i;
            }
            else
            {
                // parse not supported
                MY_LOG_INFO("%s not supported", value.c_str());
                ++i;
            }
        }
        if (parseError)
        {
            MY_PRINT_STR(helpDocument);
            exit(0);
        }
        if (includeFixPaths.empty())
        {
            // use current path
            auto currentPath = FileSystem::fs::current_path().string();
            includeFixPaths.emplace_back(currentPath);
        }
    }
}

namespace MyResult
{
    std::vector<std::string> results;
    std::string outPath = "";
    void addResult(const std::string& content)
    {
        results.emplace_back(content);
    }
    void writeResult()
    {
        auto path = FileSystem::fs::path(outPath);
        auto pathParent = path.parent_path();
        if (FileSystem::isDir(pathParent.string()))
        {
            std::vector<std::uint8_t> buffer;
            std::size_t count = 0;
            for (auto& one : results)
            {
                count = count + one.size() + 1;
            }
            buffer.resize(count, '\0');
            count = 0;
            for (auto& one : results)
            {
                std::memcpy((void*)(&(buffer[count])), one.data(), one.size());
                buffer[count + one.size()] = '\n';
                count += (one.size() + 1);
            }
            FileSystem::writeFile(path.string(), buffer);
            return;
        }

        MY_PRINT_STR("");
        MY_PRINT_STR("");
        MY_PRINT_STR("");

        for (auto& one : results)
        {
            MY_PRINT_STR(one.c_str());
        }

        MY_PRINT_STR("");
        MY_PRINT_STR("");
        MY_PRINT_STR("");
    }
}

static constexpr auto regexStr = R"regexStr(#\s*include\s*([<"][^>"]*[>"])\s*)regexStr";


int main(int argc, char** argv)
{
    ConfigureParams::parseInputParams(argc, argv);

    // parse files
    FileSystem::files.clear();
    FileSystem::includePaths.clear();

    // add system file include
    std::unordered_map<std::string, std::string> systemTempIncludePaths;
    auto selectLangVer = ConfigureParams::languageVersion;
    for (auto& one : languageCoreHeader::standardCPPHeaders)
    {
        auto fileName = std::get<0>(one);
        auto version = std::get<1>(one);
        if (version <= selectLangVer)
        {
            // use system
            systemTempIncludePaths[fileName] = "1";
        }
        else
        {
            // not use system
            systemTempIncludePaths[fileName] = "-1";
        }
    }
    for (auto& one : languageCoreHeader::standardCHeaders)
    {
        auto fileName = std::get<0>(one);
        auto version = std::get<1>(one);
        if (version <= selectLangVer)
        {
            systemTempIncludePaths[fileName] = "1";
        }
        else
        {
            systemTempIncludePaths[fileName] = "-1";
        }
    }
    for (auto& one : languageCoreHeader::standardDeprecatedCPPHeaders)
    {
        auto fileName = std::get<0>(one);
        auto precated_ver = std::get<1>(one);
        auto remove_ver = std::get<2>(one);
        if (remove_ver <= selectLangVer)
        {
            // not use system and remove(log)
            systemTempIncludePaths[fileName] = "-2";
        }
        else if (precated_ver <= selectLangVer)
        {
            // use system and depreacted
            systemTempIncludePaths[fileName] = "2";
        }
    }
    for (auto& one : languageCoreHeader::standardDeprecatedCHeaders)
    {
        auto fileName = std::get<0>(one);
        auto precated_ver = std::get<1>(one);
        auto remove_ver = std::get<2>(one);
        if (remove_ver <= selectLangVer)
        {
            // not use system and remove(log)
            systemTempIncludePaths[fileName] = "-2";
        }
        else if (precated_ver <= selectLangVer)
        {
            // use system and depreacted
            systemTempIncludePaths[fileName] = "2";
        }
    }

    // parse include paths
    std::vector<std::tuple<bool, std::string, std::string, std::string>> notSystemIncludeHeaders;
    std::vector<std::tuple<bool, std::string, std::string, std::string>> notSystemExcludeHeaders;
    for (auto& one : ConfigureParams::includePaths)
    {
        FileSystem::getChildren(one, notSystemIncludeHeaders);
    }
    for (auto& one : ConfigureParams::excludePaths)
    {
        FileSystem::getChildren(one, notSystemExcludeHeaders);
    }
    std::vector<std::tuple<std::string, std::string, std::string>> tempIncludePath;
    for (auto& one : notSystemIncludeHeaders)
    {
        auto isDir = std::get<0>(one);
        auto fileName = std::get<1>(one);
        auto fileRelativePath = std::get<2>(one);
        auto fileFullPath = std::get<3>(one);
        if (!isDir)
        {
            tempIncludePath.emplace_back(fileName, fileRelativePath, fileFullPath);
        }
    }
    // remove repeat by full path
    std::unordered_map<std::string, std::tuple<std::string, std::string>> tempIncludePath2;
    for (auto& one : tempIncludePath)
    {
        auto fileName = std::get<0>(one);
        auto fileRelativePath = std::get<1>(one);
        auto fullPath = std::get<2>(one);
        tempIncludePath2[fullPath] = std::make_tuple(fileName, fileRelativePath);
    }
    // remove from exclude by full path
    for (auto& one : notSystemExcludeHeaders)
    {
        auto isDir = std::get<0>(one);
        auto fileName = std::get<1>(one);
        auto fileRelativePath = std::get<2>(one);
        auto fileFullPath = std::get<3>(one);
        if (!isDir)
        {
            tempIncludePath2[fileFullPath] = std::make_tuple("", "");
        }
    }
    std::unordered_map<std::string, std::string> nonSystemTempIncludePaths;
    for (auto& one : tempIncludePath2)
    {
        auto fileName = std::get<0>(one.second);
        auto fileRelativePath = std::get<1>(one.second);
        if (!fileName.empty())
        {
            nonSystemTempIncludePaths[fileName] = fileRelativePath;
        }
    }
    // remove skip filename
    for (auto& one : ConfigureParams::skipFileNames)
    {
        // set ignore
        systemTempIncludePaths[one] = "-3";
        auto iter = nonSystemTempIncludePaths.find(one);
        if (iter != nonSystemTempIncludePaths.end())
        {
            nonSystemTempIncludePaths.erase(iter);
        }
    }

    // use local replace system
    for (auto& one : ConfigureParams::useLocalFileNames)
    {
        auto iter = systemTempIncludePaths.find(one);
        if (iter != systemTempIncludePaths.end())
        {
            auto iter2 = nonSystemTempIncludePaths.find(one);
            if (iter2 != nonSystemTempIncludePaths.end())
            {
                systemTempIncludePaths.erase(iter);
            }
        }
    }

    for (auto& one : nonSystemTempIncludePaths)
    {
        auto& fileName = std::get<0>(one);
        auto& fileRelativePath = std::get<1>(one);
        if (!fileName.empty() && !fileRelativePath.empty())
        {
            FileSystem::includePaths[fileName] = std::string("\"").append(fileRelativePath).append("\"");
        }
    }
    for (auto& one : systemTempIncludePaths)
    {
        auto& fileName = std::get<0>(one);
        auto& status = std::get<1>(one);
        if (!fileName.empty() && !status.empty())
        {
            // 1 normal  2 deprecated
            // -1 not compile include(version too low)  -2 remove -3 set skip
            if(status == "1" || status != "2")
            {
                FileSystem::includePaths[fileName] = std::string("<").append(fileName).append(">");
            }
        }
    }

    // check fix path
    std::vector<std::tuple<bool, std::string, std::string, std::string>> includeFixPaths;
    std::vector<std::tuple<bool, std::string, std::string, std::string>> excludeFixPaths;
    for (auto& one : ConfigureParams::includeFixPaths)
    {
        FileSystem::getChildren(one, includeFixPaths);
    }
    for (auto& one : ConfigureParams::excludeFixPaths)
    {
        FileSystem::getChildren(one, excludeFixPaths);
    }
    // key = fullPath value = relative path
    std::unordered_map<std::string, std::string> fixPaths;
    for(auto& one : includeFixPaths)
    {
        auto isDir = std::get<0>(one);
        auto fileName = std::get<1>(one);
        auto fileRelativePath = std::get<2>(one);
        auto fileFullPath = std::get<3>(one);
        if (!isDir)
        {
            for(auto& one : ConfigureParams::fileSuffix)
            {
                if(fileFullPath.ends_with(one))
                {
                    fixPaths[fileFullPath] = fileRelativePath;
                    break;
                }
            }
        }
    }
    for(auto& one : excludeFixPaths)
    {
        auto isDir = std::get<0>(one);
        auto fileName = std::get<1>(one);
        auto fileRelativePath = std::get<2>(one);
        auto fileFullPath = std::get<3>(one);
        if (!isDir)
        {
            fixPaths[fileFullPath] = "";
        }
    }

    // gen task
    class MyTask : public MyThreadPool::AsyncTask
    {
// #include <string>
// #include <iostream>
// #include <regex>
// int main() {
//     std::string s = R"(#include<aa.h>#include "cc.dd.h")";
//     auto d1 = R"regexStr(#\s*include\s*[<"]([^>"]*)[>"])regexStr";
//     std::regex r(d1,std::regex_constants::ECMAScript | std::regex_constants::icase);

//     for(std::sregex_iterator i = std::sregex_iterator(s.begin(), s.end(), r);
//                             i != std::sregex_iterator();
//                             ++i )
//     {
//         std::smatch m = *i;
//         std::cout << "match count " << m.size();
//         for(std::size_t idx = 0; idx < m.size(); idx++)
//         {
//             std::cout << ' ' << m.str(idx) << " at position " << m.position(idx);
//         }
//         std::cout << '\n';
//     }
//     std::cout << "finish\n" << d1;
// }
// Output:
// match count 2 #include<aa.h> at position 0 aa.h at position 9
// match count 2 #include "cc.dd.h" at position 14 cc.dd.h at position 24
// finish
// #\s*include\s*[<"]([^>"]*)[>"]
    public:
        virtual void run() override
        {
            std::vector<std::uint8_t> fileContent;
            FileSystem::readFile(fullPath, fileContent);
            auto fileSize = fileContent.size();

            std::vector<std::uint8_t> newLineContent;
            newLineContent.resize(fileSize + 2048,'\0');
            std::uint32_t newContentSize = 0;
            std::uint32_t firstPos = 0;
            std::uint32_t endPos = 0xffffffff;
            std::regex includeMatchRegex(regexStr,std::regex_constants::ECMAScript | std::regex_constants::icase);

            std::uint32_t currentLine = 1;
            std::uint32_t curCharIdx = 0;
            std::uint32_t lineMatchBegin = 0;
            std::uint32_t lineMatchEnd = 0;
            bool isChange = false;
            while(curCharIdx < fileSize)
            {
                while(curCharIdx < fileSize && fileContent[curCharIdx] != '\n')
                {
                    ++curCharIdx;
                }
                if(curCharIdx < fileSize)
                {
                    // get new line
                    endPos = curCharIdx;
                }
                else
                {
                    // get last line
                    endPos = fileSize;
                }
                lineMatchBegin = 0;
                lineMatchEnd = 0;
                std::string line((const char*)(&fileContent[firstPos]),endPos - firstPos + (curCharIdx < fileSize ? 1 : 0));
                std::string newLine;
                auto lineMatchBeginIt = std::sregex_iterator(line.begin(), line.end(), includeMatchRegex);
                auto lineMatchEndIt = std::sregex_iterator();
                for (auto matchIdx = lineMatchBeginIt; matchIdx != lineMatchEndIt; ++matchIdx) {
                    auto& match = *matchIdx;                                                 
                    if(match.size() == 2)
                    {
                        auto matchStr = match.str(1);
                        // remove "" or <>
                        matchStr = matchStr.substr(1, matchStr.size() - 2);
                        // find last name
                        auto last1 = matchStr.find_last_of('/');
                        auto last2 = matchStr.find_last_of('\\');
                        std::size_t lastIndex = 0xffffffff - 1;
                        if(last1 != std::string::npos && last1 < lastIndex)
                        {
                            lastIndex = last1;
                        }
                        if(last2 != std::string::npos && last2 < lastIndex)
                        {
                            lastIndex = last2;
                        }
                        if(lastIndex + 1 < matchStr.size())
                        {
                            matchStr = matchStr.substr(lastIndex + 1);
                        }
                        // match
                        auto matchStartPos = match.position(1);
                        auto matchLendth = match.length(1);
                        auto replaceStr = FileSystem::includePaths[matchStr];
                        if(replaceStr.size() > 0 && match.str(1) != replaceStr )
                        {
                            if(!isChange)
                            {
                                isChange = true;
                            }
                            // append prev
                            if(matchStartPos > lineMatchEnd)
                            {
                                newLine.append((const char*)(&line[lineMatchEnd]),matchStartPos - lineMatchEnd);
                            }

                            // append replace
                            lineMatchBegin = matchStartPos;
                            lineMatchEnd = lineMatchBegin + matchLendth;
                            newLine.append(replaceStr);

                            taskLog.append("@line:").append(std::to_string(currentLine));
                            taskLog.append("@pos:").append(std::to_string(lineMatchBegin)).append(" ");
                            taskLog.append(matchStr).append(" replace to ").append(replaceStr);
                            taskLog.append("\n");
                        }
                        else
                        {
                            taskLog.append("@line:").append(std::to_string(currentLine));
                            taskLog.append("@pos:").append(std::to_string(lineMatchBegin)).append(" ");
                            taskLog.append(matchStr).append(" not found replace");
                            taskLog.append("\n");
                        }
                    }
                }
                // append last
                if(line.size() > lineMatchEnd)
                {
                    newLine.append((const char*)(&line[lineMatchEnd]),line.size() - lineMatchEnd);
                }
                while(newContentSize + newLine.size() > newLineContent.size())
                {
                    newLineContent.resize(newContentSize + newLine.size() + 1024);
                }
                std::memcpy(&newLineContent[newContentSize],newLine.data(),newLine.size());
                newContentSize += newLine.size();
                currentLine++;
                firstPos = endPos + 1;
                ++curCharIdx;
            }
            if(isChange)
            {
                newLineContent.resize(newContentSize);
                FileSystem::writeFile(fullPath, newLineContent);
            }
        }

        virtual void onStart() override
        {
            taskLog = "";
        }

        virtual void onEnd() override
        {
            auto myLog = std::string("");
            myLog.append("current file: ").append(fullPath);
            myLog.append("log:\n").append(taskLog);
            MyResult::addResult(myLog); 
        }
        std::string fullPath;
        std::string taskLog = "";
    };
    MyThreadPool::ThreadPool threadpool;
    threadpool.maxThreadCount = ConfigureParams::maxThread;
    bool isFinish = false;
    for(auto& one : fixPaths)
    {
        auto& fullPath = one.first;
        auto& relativePath = one.second;
        if(!relativePath.empty())
        {
            auto task = new MyTask();
            task->fullPath = fullPath; 
            threadpool.addTask(task);
        }
    }
    while(threadpool.taskSize() > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        threadpool.tick();
        isFinish = threadpool.taskSize() == 0;
    }

    MyResult::writeResult();

    if(!ConfigureParams::disablePause)
    {
        std::cout << "press any key..." << std::endl;
        std::cin.ignore();
    }
    return 0;
}
