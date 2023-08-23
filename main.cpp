#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <sstream>

namespace CommandLineParameter {
    inline constexpr auto Threads = "--threads";
    inline constexpr auto Delay = "--delay";
}

namespace ConsoleCommand {
    inline constexpr auto Info = "info";
    inline constexpr auto NewThread = "new";
    inline constexpr auto KillThread = "kill";
    inline constexpr auto ResetThread = "reset";
    inline constexpr auto Stop = "stop";
}

struct ThreadData {
    bool killed;
    int64_t value;
};

using namespace std::chrono_literals;

std::atomic_bool programFinished{false};

std::mutex threadPoolMutex;
std::list<std::thread> runningThreads;

std::mutex threadDataMutex;
std::map<std::thread::id, ThreadData> threadsData;

std::pair<size_t, std::chrono::seconds> parseCommandLine(int argc, char *argv[]) {
    size_t numberOfThreads = std::thread::hardware_concurrency();
    auto threadStartDelay = 1s;

    const auto convertArgumentToPositiveInteger = [](const auto arg) {
        const auto value = std::stoi(arg);
        if (value < 0) {
            throw std::logic_error("Parameter value must be a positive integer number");
        }
        return value;
    };

    try {
        for (auto i = 1; i < argc; ++i) {
            std::string argument = argv[i];
            if (argument == CommandLineParameter::Threads && i + 1 < argc) {
                numberOfThreads = convertArgumentToPositiveInteger(argv[++i]);
            } else if (argument == CommandLineParameter::Delay && i + 1 < argc) {
                threadStartDelay = std::chrono::seconds(convertArgumentToPositiveInteger(argv[++i]));
            } else {
                throw std::logic_error("Unknown command line parameter " + argument);
            }
        }
    } catch (std::invalid_argument &e) {
        std::cout << "Value of an argument is not a number";
        exit(1);
    } catch (std::logic_error &e) {
        std::cout << e.what() << std::endl;
        exit(1);
    }

    return {numberOfThreads, threadStartDelay};
}

int64_t initializeThreadValue(std::optional<int64_t> initialValue) {
    if (initialValue.has_value()) {
        return initialValue.value();
    }
    std::srand(time(nullptr)); // NOLINT(cert-msc51-cpp)
    return std::rand(); // NOLINT(cert-msc50-cpp)
}

void startNewThread(std::optional<int64_t> initialValue = std::nullopt) {
    std::lock_guard lock(threadPoolMutex);
    runningThreads.emplace_back([](const auto initialValue) {
        const auto threadId = std::this_thread::get_id();
        auto threadValue = initializeThreadValue(initialValue);
        {
            std::lock_guard lock(threadDataMutex);
            threadsData.insert({threadId, {false, threadValue}});
        }
        std::cout << "Thread (id=" << threadId << ") was started, my init value =" << threadValue << std::endl;
        while (!programFinished) {
            std::this_thread::sleep_for(1s);
            {
                std::lock_guard lock(threadDataMutex);
                auto &threadData = threadsData.at(threadId);
                if (threadData.killed) {
                    break;
                }
                threadValue = ++threadData.value;
            }
        }
        std::cout << "Thread (id=" << threadId << ") was finished, value = " << threadValue << std::endl;
    }, initialValue);
}

void printThreadsInfo() {
    std::lock_guard lock_pool(threadPoolMutex);
    std::lock_guard lock_data(threadDataMutex);
    for (const auto &thread: runningThreads) {
        const auto id = thread.get_id();
        std::cout << "Thread (id=" << id << "), value = " << threadsData.at(id).value << std::endl;
    }
}

void killThread(size_t id) {
    const auto idToKill = std::thread::id(id);
    {
        std::lock_guard lock_data(threadDataMutex);
        if (threadsData.find(idToKill) == threadsData.end()) {
            return;
        }
        threadsData.at(idToKill).killed = true;
    }
    auto threadIt = runningThreads.end();
    {
        std::lock_guard lock_pool(threadPoolMutex);
        threadIt = std::find_if(runningThreads.begin(), runningThreads.end(),[&idToKill](const auto &thread) {
            return thread.get_id() == idToKill;
        });
    }
    if (threadIt != runningThreads.end()) {
        threadIt->join();
    }
    {
        std::lock_guard lock_data(threadDataMutex);
        threadsData.erase(idToKill);
        std::lock_guard lock_pool(threadPoolMutex);
        runningThreads.erase(threadIt);
    }
}

void resetThread(size_t id, int64_t newValue) {
    const auto idToReset = std::thread::id(id);
    {
        std::lock_guard lock_data(threadDataMutex);
        if (threadsData.find(idToReset) == threadsData.end()) {
            return;
        }
        threadsData.at(idToReset).value = newValue;
    }
    std::cout << "Thread (id=" << idToReset << "), new value is " << newValue << std::endl;
}

std::thread launchThreads(size_t numberOfThreads, std::chrono::seconds threadStartDelay) {
    return std::thread([](const auto number, const auto delay) {
        std::cout << "Thread initializer thread started." << std::endl;
        for (auto i = 0; i < number && !programFinished; ++i) {
            startNewThread();
            if (i < (number - 1)) {
                std::this_thread::sleep_for(delay);
            }
        }
        std::cout << "Thread initializer thread finished." << std::endl;
    },numberOfThreads, threadStartDelay);
}

void stopRunningThreads() {
    std::lock_guard lock(threadPoolMutex);
    for (auto &thread: runningThreads) {
        thread.join();
    }
}

std::pair<std::string, std::vector<std::string>> parseCommand(const std::string &line) {
    std::string command{};
    std::vector<std::string> arguments{};

    std::istringstream iss(line);
    iss >> command;

    std::string argument;
    while (iss >> argument) {
        arguments.push_back(argument);
    }

    return {command, arguments};
}

void invokeCommand(const std::string &line) {
    const auto [command, arguments] = parseCommand(line);
    if (command == ConsoleCommand::Info) {
        printThreadsInfo();
    } else if (command == ConsoleCommand::NewThread) {
        std::optional<int64_t> value = std::nullopt;
        try { value = std::stoi(arguments.at(0)); } catch (...) {}
        startNewThread(value);
    } else if (command == ConsoleCommand::KillThread) {
        size_t id;
        try { id = std::stoi(arguments.at(0)); }
        catch (...) {
            std::cout << "Please provide thread id" << std::endl;
            return;
        }
        killThread(id);
    } else if (command == ConsoleCommand::ResetThread) {
        size_t id;
        int64_t newValue{0};
        try { id = std::stoi(arguments.at(0)); }
        catch (...) {
            std::cout << "Please provide thread id" << std::endl;
            return;
        }
        try { newValue = std::stoi(arguments.at(1)); }
        catch (...) {}
        resetThread(id, newValue);
    } else if (command == ConsoleCommand::Stop) {
        programFinished = true;
        stopRunningThreads();
    } else {
        std::cout << "Unknown command." << std::endl;
    }
}

void listenToCommandLine() {
    std::string line;
    while (!programFinished) {
        std::getline(std::cin, line);
        invokeCommand(line);
    }
}

int main(int argc, char *argv[]) {
    const auto [numberOfThreads, threadStartDelay] = parseCommandLine(argc, argv);
    auto threadCreatorThread = launchThreads(numberOfThreads, threadStartDelay);
    listenToCommandLine();
    threadCreatorThread.join();
    return 0;
}
