// ============================================================================
// ProsperoLayer RDNA2 Core - System Services Header
// ============================================================================
// Description: HLE implementation of PS5 system services
// ============================================================================

#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/resource.h>

// ============================================================================
// Forward Declarations
// ============================================================================

namespace ProsperoLayer {

// ============================================================================
// Enums
// ============================================================================

enum class ProcessState {
    Created,
    Running,
    Suspended,
    Terminated
};

enum class ThreadState {
    Created,
    Running,
    Suspended,
    Terminated
};

// ============================================================================
// Structures
// ============================================================================

struct SystemInfo {
    std::string system_version;
    std::string system_name;
    uint64_t total_memory;
    uint64_t available_memory;
    int cpu_cores;
    int cpu_frequency;
    uint64_t gpu_memory;
    std::string os_version;
};

struct ProcessCreateInfo {
    std::string name;
    int priority;
    size_t stack_size;
    void* entry_point;
    void* argument;
};

struct ProcessInfo {
    int pid;
    std::string name;
    ProcessState state;
    int priority;
    std::chrono::steady_clock::time_point creation_time;
    std::chrono::steady_clock::time_point termination_time;
};

struct ThreadCreateInfo {
    std::string name;
    int priority;
    size_t stack_size;
    void* entry_point;
    void* argument;
};

struct ThreadInfo {
    int tid;
    std::string name;
    ThreadState state;
    int priority;
    size_t stack_size;
    void* entry_point;
    void* argument;
    std::chrono::steady_clock::time_point creation_time;
    std::chrono::steady_clock::time_point termination_time;
};

struct MemoryAllocation {
    void* address;
    size_t size;
    int protection;
    std::chrono::steady_clock::time_point allocation_time;
};

struct MutexCreateInfo {
    std::string name;
    bool recursive;
};

struct Mutex {
    int id;
    std::string name;
    int owner;
    int lock_count;
    bool recursive;
    pthread_mutex_t native_mutex;
};

struct FileDescriptor {
    int fd;
    std::string path;
    int flags;
    off_t position;
    std::chrono::steady_clock::time_point open_time;
};

struct Configuration {
    int max_processes;
    int max_threads;
    int max_open_files;
    size_t stack_size;
    size_t heap_size;
};

struct SystemStatistics {
    uint64_t total_processes_created;
    size_t active_processes;
    uint64_t total_threads_created;
    size_t active_threads;
    size_t total_memory_allocated;
    size_t total_files_opened;
    uint64_t total_mutexes_created;
    uint64_t uptime;
};

// ============================================================================
// System Services Class
// ============================================================================

class SystemServices {
public:
    // Singleton pattern
    static SystemServices& GetInstance() {
        static SystemServices instance;
        return instance;
    }
    
    // Delete copy and move constructors
    SystemServices(const SystemServices&) = delete;
    SystemServices& operator=(const SystemServices&) = delete;
    SystemServices(SystemServices&&) = delete;
    SystemServices& operator=(SystemServices&&) = delete;
    
    // System Information
    SystemInfo GetSystemInfo();
    
    // Process Management
    int GetCurrentProcessId();
    int GetCurrentThreadId();
    int CreateProcess(const ProcessCreateInfo& create_info);
    bool TerminateProcess(int pid);
    std::vector<ProcessInfo> GetProcessList();
    
    // Thread Management
    int CreateThread(const ThreadCreateInfo& create_info);
    bool TerminateThread(int tid);
    std::vector<ThreadInfo> GetThreadList();
    
    // Memory Management
    void* AllocateMemory(size_t size, int protection = PROT_READ | PROT_WRITE);
    bool FreeMemory(void* ptr);
    bool ProtectMemory(void* ptr, size_t size, int protection);
    size_t GetTotalAllocatedMemory() const;
    
    // Synchronization
    int CreateMutex(const MutexCreateInfo& create_info);
    bool LockMutex(int mutex_id);
    bool UnlockMutex(int mutex_id);
    bool DestroyMutex(int mutex_id);
    
    // File System
    int OpenFile(const std::string& path, int flags, int mode = 0644);
    bool CloseFile(int fd);
    ssize_t ReadFile(int fd, void* buffer, size_t size);
    ssize_t WriteFile(int fd, const void* buffer, size_t size);
    off_t SeekFile(int fd, off_t offset, int whence);
    
    // Time and Timer
    uint64_t GetTickCount() const;
    uint64_t GetPerformanceCounter();
    uint64_t GetPerformanceFrequency();
    bool Sleep(uint64_t milliseconds);
    
    // Error Handling
    int GetLastError() const;
    void SetLastError(int error);
    std::string GetErrorMessage(int error) const;
    
    // Configuration
    bool SetConfiguration(const Configuration& config);
    Configuration GetConfiguration() const;
    
    // Statistics
    SystemStatistics GetStatistics() const;
    void ResetStatistics();
    
    // Initialization and Cleanup
    bool Initialize();
    void Shutdown();

private:
    // Private constructor for singleton
    SystemServices() = default;
    ~SystemServices() = default;
    
    // Member variables
    int next_process_id_ = 1;
    int next_thread_id_ = 1;
    int next_mutex_id_ = 1;
    size_t total_allocated_ = 0;
    int last_error_ = 0;
    
    std::map<int, ProcessInfo> processes_;
    std::map<int, ThreadInfo> threads_;
    std::map<int, Mutex> mutexes_;
    std::map<int, FileDescriptor> files_;
    std::map<void*, MemoryAllocation> allocations_;
    
    Configuration configuration_{};
    
    // Helper methods
    void LogSystemInfo(const SystemInfo& info);
    void LogProcessInfo(const ProcessInfo& info);
    void LogThreadInfo(const ThreadInfo& info);
};

} // namespace ProsperoLayer