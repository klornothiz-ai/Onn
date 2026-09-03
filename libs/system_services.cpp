// ============================================================================
// ProsperoLayer RDNA2 Core - System Services Library
// ============================================================================
// Description: HLE implementation of PS5 system services
// ============================================================================

#include "kernel/system_services.h"
#include "common/logging/log.h"

// ============================================================================
// System Services Implementation
// ============================================================================

namespace ProsperoLayer {

// ----------------------------------------------------------------------------
// System Information
// ----------------------------------------------------------------------------

SystemInfo SystemServices::GetSystemInfo() {
    LOG_INFO("SystemServices::GetSystemInfo called");
    
    SystemInfo info{};
    info.system_version = "5.00";
    info.system_name = "ProsperoLayer Emulator";
    info.total_memory = 16ULL * 1024 * 1024 * 1024; // 16GB
    info.available_memory = info.total_memory;
    info.cpu_cores = std::thread::hardware_concurrency();
    info.cpu_frequency = 3500; // 3.5 GHz
    info.gpu_memory = 16ULL * 1024 * 1024 * 1024; // 16GB
    info.os_version = "ProsperoLayer OS 1.0";
    
    return info;
}

// ----------------------------------------------------------------------------
// Process Management
// ----------------------------------------------------------------------------

int SystemServices::GetCurrentProcessId() {
    LOG_DEBUG("SystemServices::GetCurrentProcessId called");
    return getpid();
}

int SystemServices::GetCurrentThreadId() {
    LOG_DEBUG("SystemServices::GetCurrentThreadId called");
    return pthread_self();
}

int SystemServices::CreateProcess(const ProcessCreateInfo& create_info) {
    LOG_INFO("SystemServices::CreateProcess called");
    
    // Validate parameters
    if (create_info.name.empty()) {
        LOG_ERROR("Process name cannot be empty");
        return -1;
    }
    
    // Create process structure
    ProcessInfo process{};
    process.pid = next_process_id_++;
    process.name = create_info.name;
    process.state = ProcessState::Created;
    process.priority = create_info.priority;
    process.creation_time = std::chrono::steady_clock::now();
    
    // Store process
    processes_[process.pid] = process;
    
    LOG_INFO("Process created: {} (PID: {})", process.name, process.pid);
    
    return process.pid;
}

bool SystemServices::TerminateProcess(int pid) {
    LOG_INFO("SystemServices::TerminateProcess called for PID: {}", pid);
    
    auto it = processes_.find(pid);
    if (it == processes_.end()) {
        LOG_ERROR("Process not found: {}", pid);
        return false;
    }
    
    it->second.state = ProcessState::Terminated;
    it->second.termination_time = std::chrono::steady_clock::now();
    
    LOG_INFO("Process terminated: {} (PID: {})", it->second.name, pid);
    
    return true;
}

std::vector<ProcessInfo> SystemServices::GetProcessList() {
    LOG_DEBUG("SystemServices::GetProcessList called");
    
    std::vector<ProcessInfo> list;
    for (const auto& [pid, process] : processes_) {
        list.push_back(process);
    }
    
    return list;
}

// ----------------------------------------------------------------------------
// Thread Management
// ----------------------------------------------------------------------------

int SystemServices::CreateThread(const ThreadCreateInfo& create_info) {
    LOG_INFO("SystemServices::CreateThread called");
    
    // Validate parameters
    if (create_info.name.empty()) {
        LOG_ERROR("Thread name cannot be empty");
        return -1;
    }
    
    // Create thread structure
    ThreadInfo thread{};
    thread.tid = next_thread_id_++;
    thread.name = create_info.name;
    thread.state = ThreadState::Created;
    thread.priority = create_info.priority;
    thread.stack_size = create_info.stack_size;
    thread.entry_point = create_info.entry_point;
    thread.argument = create_info.argument;
    thread.creation_time = std::chrono::steady_clock::now();
    
    // Store thread
    threads_[thread.tid] = thread;
    
    LOG_INFO("Thread created: {} (TID: {})", thread.name, thread.tid);
    
    return thread.tid;
}

bool SystemServices::TerminateThread(int tid) {
    LOG_INFO("SystemServices::TerminateThread called for TID: {}", tid);
    
    auto it = threads_.find(tid);
    if (it == threads_.end()) {
        LOG_ERROR("Thread not found: {}", tid);
        return false;
    }
    
    it->second.state = ThreadState::Terminated;
    it->second.termination_time = std::chrono::steady_clock::now();
    
    LOG_INFO("Thread terminated: {} (TID: {})", it->second.name, tid);
    
    return true;
}

std::vector<ThreadInfo> SystemServices::GetThreadList() {
    LOG_DEBUG("SystemServices::GetThreadList called");
    
    std::vector<ThreadInfo> list;
    for (const auto& [tid, thread] : threads_) {
        list.push_back(thread);
    }
    
    return list;
}

// ----------------------------------------------------------------------------
// Memory Management
// ----------------------------------------------------------------------------

void* SystemServices::AllocateMemory(size_t size, int protection) {
    LOG_DEBUG("SystemServices::AllocateMemory called, size: {}", size);
    
    // Validate parameters
    if (size == 0) {
        LOG_ERROR("Allocation size cannot be zero");
        return nullptr;
    }
    
    // Allocate memory
    void* ptr = mmap(nullptr, size, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        LOG_ERROR("Memory allocation failed: {}", strerror(errno));
        return nullptr;
    }
    
    // Track allocation
    MemoryAllocation allocation{};
    allocation.address = ptr;
    allocation.size = size;
    allocation.protection = protection;
    allocation.allocation_time = std::chrono::steady_clock::now();
    
    allocations_[ptr] = allocation;
    total_allocated_ += size;
    
    LOG_DEBUG("Memory allocated: {} bytes at {:p}", size, ptr);
    
    return ptr;
}

bool SystemServices::FreeMemory(void* ptr) {
    LOG_DEBUG("SystemServices::FreeMemory called for {:p}", ptr);
    
    auto it = allocations_.find(ptr);
    if (it == allocations_.end()) {
        LOG_ERROR("Memory not found: {:p}", ptr);
        return false;
    }
    
    // Free memory
    if (munmap(ptr, it->second.size) != 0) {
        LOG_ERROR("Memory free failed: {}", strerror(errno));
        return false;
    }
    
    total_allocated_ -= it->second.size;
    allocations_.erase(it);
    
    LOG_DEBUG("Memory freed: {:p}", ptr);
    
    return true;
}

bool SystemServices::ProtectMemory(void* ptr, size_t size, int protection) {
    LOG_DEBUG("SystemServices::ProtectMemory called for {:p}, size: {}", ptr, size);
    
    auto it = allocations_.find(ptr);
    if (it == allocations_.end()) {
        LOG_ERROR("Memory not found: {:p}", ptr);
        return false;
    }
    
    // Change memory protection
    if (mprotect(ptr, size, protection) != 0) {
        LOG_ERROR("Memory protect failed: {}", strerror(errno));
        return false;
    }
    
    it->second.protection = protection;
    
    LOG_DEBUG("Memory protection changed: {:p}", ptr);
    
    return true;
}

size_t SystemServices::GetTotalAllocatedMemory() const {
    return total_allocated_;
}

// ----------------------------------------------------------------------------
// Synchronization
// ----------------------------------------------------------------------------

int SystemServices::CreateMutex(const MutexCreateInfo& create_info) {
    LOG_DEBUG("SystemServices::CreateMutex called");
    
    // Create mutex structure
    Mutex mutex{};
    mutex.id = next_mutex_id_++;
    mutex.name = create_info.name;
    mutex.owner = 0;
    mutex.lock_count = 0;
    mutex.recursive = create_info.recursive;
    
    // Initialize mutex attributes
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    
    if (mutex.recursive) {
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    } else {
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
    }
    
    pthread_mutex_init(&mutex.native_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    
    // Store mutex
    mutexes_[mutex.id] = mutex;
    
    LOG_DEBUG("Mutex created: {} (ID: {})", mutex.name, mutex.id);
    
    return mutex.id;
}

bool SystemServices::LockMutex(int mutex_id) {
    LOG_DEBUG("SystemServices::LockMutex called for ID: {}", mutex_id);
    
    auto it = mutexes_.find(mutex_id);
    if (it == mutexes_.end()) {
        LOG_ERROR("Mutex not found: {}", mutex_id);
        return false;
    }
    
    int result = pthread_mutex_lock(&it->second.native_mutex);
    if (result != 0) {
        LOG_ERROR("Mutex lock failed: {}", strerror(result));
        return false;
    }
    
    it->second.owner = GetCurrentThreadId();
    it->second.lock_count++;
    
    LOG_DEBUG("Mutex locked: {} (ID: {})", it->second.name, mutex_id);
    
    return true;
}

bool SystemServices::UnlockMutex(int mutex_id) {
    LOG_DEBUG("SystemServices::UnlockMutex called for ID: {}", mutex_id);
    
    auto it = mutexes_.find(mutex_id);
    if (it == mutexes_.end()) {
        LOG_ERROR("Mutex not found: {}", mutex_id);
        return false;
    }
    
    int result = pthread_mutex_unlock(&it->second.native_mutex);
    if (result != 0) {
        LOG_ERROR("Mutex unlock failed: {}", strerror(result));
        return false;
    }
    
    it->second.lock_count--;
    if (it->second.lock_count == 0) {
        it->second.owner = 0;
    }
    
    LOG_DEBUG("Mutex unlocked: {} (ID: {})", it->second.name, mutex_id);
    
    return true;
}

bool SystemServices::DestroyMutex(int mutex_id) {
    LOG_DEBUG("SystemServices::DestroyMutex called for ID: {}", mutex_id);
    
    auto it = mutexes_.find(mutex_id);
    if (it == mutexes_.end()) {
        LOG_ERROR("Mutex not found: {}", mutex_id);
        return false;
    }
    
    int result = pthread_mutex_destroy(&it->second.native_mutex);
    if (result != 0) {
        LOG_ERROR("Mutex destroy failed: {}", strerror(result));
        return false;
    }
    
    mutexes_.erase(it);
    
    LOG_DEBUG("Mutex destroyed: ID: {}", mutex_id);
    
    return true;
}

// ----------------------------------------------------------------------------
// File System
// ----------------------------------------------------------------------------

int SystemServices::OpenFile(const std::string& path, int flags, int mode) {
    LOG_DEBUG("SystemServices::OpenFile called: {}", path);
    
    // Validate parameters
    if (path.empty()) {
        LOG_ERROR("File path cannot be empty");
        return -1;
    }
    
    // Open file
    int fd = open(path.c_str(), flags, mode);
    if (fd == -1) {
        LOG_ERROR("File open failed: {}", strerror(errno));
        return -1;
    }
    
    // Track file
    FileDescriptor file{};
    file.fd = fd;
    file.path = path;
    file.flags = flags;
    file.position = 0;
    file.open_time = std::chrono::steady_clock::now();
    
    files_[fd] = file;
    
    LOG_DEBUG("File opened: {} (FD: {})", path, fd);
    
    return fd;
}

bool SystemServices::CloseFile(int fd) {
    LOG_DEBUG("SystemServices::CloseFile called for FD: {}", fd);
    
    auto it = files_.find(fd);
    if (it == files_.end()) {
        LOG_ERROR("File not found: FD {}", fd);
        return false;
    }
    
    if (close(fd) != 0) {
        LOG_ERROR("File close failed: {}", strerror(errno));
        return false;
    }
    
    files_.erase(it);
    
    LOG_DEBUG("File closed: FD {}", fd);
    
    return true;
}

ssize_t SystemServices::ReadFile(int fd, void* buffer, size_t size) {
    LOG_DEBUG("SystemServices::ReadFile called for FD: {}, size: {}", fd, size);
    
    auto it = files_.find(fd);
    if (it == files_.end()) {
        LOG_ERROR("File not found: FD {}", fd);
        return -1;
    }
    
    ssize_t bytes_read = read(fd, buffer, size);
    if (bytes_read == -1) {
        LOG_ERROR("File read failed: {}", strerror(errno));
        return -1;
    }
    
    it->second.position += bytes_read;
    
    LOG_DEBUG("File read: {} bytes from FD {}", bytes_read, fd);
    
    return bytes_read;
}

ssize_t SystemServices::WriteFile(int fd, const void* buffer, size_t size) {
    LOG_DEBUG("SystemServices::WriteFile called for FD: {}, size: {}", fd, size);
    
    auto it = files_.find(fd);
    if (it == files_.end()) {
        LOG_ERROR("File not found: FD {}", fd);
        return -1;
    }
    
    ssize_t bytes_written = write(fd, buffer, size);
    if (bytes_written == -1) {
        LOG_ERROR("File write failed: {}", strerror(errno));
        return -1;
    }
    
    it->second.position += bytes_written;
    
    LOG_DEBUG("File written: {} bytes to FD {}", bytes_written, fd);
    
    return bytes_written;
}

off_t SystemServices::SeekFile(int fd, off_t offset, int whence) {
    LOG_DEBUG("SystemServices::SeekFile called for FD: {}, offset: {}", fd, offset);
    
    auto it = files_.find(fd);
    if (it == files_.end()) {
        LOG_ERROR("File not found: FD {}", fd);
        return -1;
    }
    
    off_t new_position = lseek(fd, offset, whence);
    if (new_position == -1) {
        LOG_ERROR("File seek failed: {}", strerror(errno));
        return -1;
    }
    
    it->second.position = new_position;
    
    LOG_DEBUG("File seeked: FD {} to position {}", fd, new_position);
    
    return new_position;
}

// ----------------------------------------------------------------------------
// Time and Timer
// ----------------------------------------------------------------------------

uint64_t SystemServices::GetTickCount() const {
    LOG_DEBUG("SystemServices::GetTickCount called");
    
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    
    return milliseconds.count();
}

uint64_t SystemServices::GetPerformanceCounter() {
    LOG_DEBUG("SystemServices::GetPerformanceCounter called");
    
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
    
    return ticks.count();
}

uint64_t SystemServices::GetPerformanceFrequency() {
    LOG_DEBUG("SystemServices::GetPerformanceFrequency called");
    
    // Return frequency in Hz
    return 1000000000; // 1 GHz
}

bool SystemServices::Sleep(uint64_t milliseconds) {
    LOG_DEBUG("SystemServices::Sleep called for {} ms", milliseconds);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    
    return true;
}

// ----------------------------------------------------------------------------
// Error Handling
// ----------------------------------------------------------------------------

int SystemServices::GetLastError() const {
    LOG_DEBUG("SystemServices::GetLastError called");
    return last_error_;
}

void SystemServices::SetLastError(int error) {
    LOG_DEBUG("SystemServices::SetLastError called with {}", error);
    last_error_ = error;
}

std::string SystemServices::GetErrorMessage(int error) const {
    LOG_DEBUG("SystemServices::GetErrorMessage called for error: {}", error);
    
    switch (error) {
        case 0:
            return "Success";
        case -1:
            return "General error";
        case -2:
            return "Invalid parameter";
        case -3:
            return "Resource not found";
        case -4:
            return "Resource already exists";
        case -5:
            return "Permission denied";
        case -6:
            return "Insufficient resources";
        case -7:
            return "Operation not supported";
        case -8:
            return "Operation timed out";
        case -9:
            return "Operation cancelled";
        default:
            return "Unknown error: " + std::to_string(error);
    }
}

// ----------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------

bool SystemServices::SetConfiguration(const Configuration& config) {
    LOG_INFO("SystemServices::SetConfiguration called");
    
    // Validate configuration
    if (config.max_processes <= 0) {
        LOG_ERROR("Invalid max processes: {}", config.max_processes);
        return false;
    }
    
    if (config.max_threads <= 0) {
        LOG_ERROR("Invalid max threads: {}", config.max_threads);
        return false;
    }
    
    // Apply configuration
    configuration_ = config;
    
    // Apply resource limits
    struct rlimit rl;
    
    // Set max processes
    rl.rlim_cur = config.max_processes;
    rl.rlim_max = config.max_processes;
    setrlimit(RLIMIT_NPROC, &rl);
    
    // Set max open files
    rl.rlim_cur = config.max_open_files;
    rl.rlim_max = config.max_open_files;
    setrlimit(RLIMIT_NOFILE, &rl);
    
    LOG_INFO("Configuration applied successfully");
    
    return true;
}

Configuration SystemServices::GetConfiguration() const {
    LOG_DEBUG("SystemServices::GetConfiguration called");
    return configuration_;
}

// ----------------------------------------------------------------------------
// Statistics
// ----------------------------------------------------------------------------

SystemStatistics SystemServices::GetStatistics() const {
    LOG_DEBUG("SystemServices::GetStatistics called");
    
    SystemStatistics stats{};
    stats.total_processes_created = next_process_id_ - 1;
    stats.active_processes = processes_.size();
    stats.total_threads_created = next_thread_id_ - 1;
    stats.active_threads = threads_.size();
    stats.total_memory_allocated = total_allocated_;
    stats.total_files_opened = files_.size();
    stats.total_mutexes_created = next_mutex_id_ - 1;
    stats.uptime = GetTickCount();
    
    return stats;
}

void SystemServices::ResetStatistics() {
    LOG_INFO("SystemServices::ResetStatistics called");
    
    next_process_id_ = 1;
    next_thread_id_ = 1;
    next_mutex_id_ = 1;
    total_allocated_ = 0;
    
    processes_.clear();
    threads_.clear();
    mutexes_.clear();
    files_.clear();
    allocations_.clear();
    
    LOG_INFO("Statistics reset");
}

// ----------------------------------------------------------------------------
// Initialization and Cleanup
// ----------------------------------------------------------------------------

bool SystemServices::Initialize() {
    LOG_INFO("SystemServices::Initialize called");
    
    // Set default configuration
    Configuration default_config{};
    default_config.max_processes = 1024;
    default_config.max_threads = 4096;
    default_config.max_open_files = 1024;
    default_config.stack_size = 8 * 1024 * 1024; // 8MB
    default_config.heap_size = 1ULL * 1024 * 1024 * 1024; // 1GB
    
    if (!SetConfiguration(default_config)) {
        LOG_ERROR("Failed to set default configuration");
        return false;
    }
    
    // Initialize statistics
    ResetStatistics();
    
    LOG_INFO("SystemServices initialized successfully");
    
    return true;
}

void SystemServices::Shutdown() {
    LOG_INFO("SystemServices::Shutdown called");
    
    // Terminate all processes
    for (auto& [pid, process] : processes_) {
        if (process.state != ProcessState::Terminated) {
            process.state = ProcessState::Terminated;
            process.termination_time = std::chrono::steady_clock::now();
        }
    }
    
    // Terminate all threads
    for (auto& [tid, thread] : threads_) {
        if (thread.state != ThreadState::Terminated) {
            thread.state = ThreadState::Terminated;
            thread.termination_time = std::chrono::steady_clock::now();
        }
    }
    
    // Destroy all mutexes
    for (auto& [id, mutex] : mutexes_) {
        pthread_mutex_destroy(&mutex.native_mutex);
    }
    
    // Close all files
    for (auto& [fd, file] : files_) {
        close(fd);
    }
    
    // Free all memory
    for (auto& [ptr, allocation] : allocations_) {
        munmap(ptr, allocation.size);
    }
    
    // Clear all collections
    processes_.clear();
    threads_.clear();
    mutexes_.clear();
    files_.clear();
    allocations_.clear();
    
    LOG_INFO("SystemServices shutdown complete");
}

} // namespace ProsperoLayer