#pragma once
#include "common/common.h"
#include <cstring>
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include <iostream>

namespace Loader {

    enum class SymbolType {
        Func,
        Object,
        NoType,
    };

    struct SymbolRecord {
        const char* name{nullptr};
        const char* library{nullptr};
        int library_version{0};
        const char* module{nullptr};
        int module_version_major{0};
        int module_version_minor{0};
        SymbolType type{SymbolType::Func};
        uint64_t   address{0};
    };

    struct SymbolResolve {
        const char* name{nullptr};
        const char* library{nullptr};
        int library_version{0};
        const char* module{nullptr};
        int module_version_major{0};
        int module_version_minor{0};
        SymbolType type{SymbolType::Func};
    };

    class SymbolDatabase {
    public:
        static SymbolDatabase& Instance() {
            static SymbolDatabase instance;
            return instance;
        }

        SymbolDatabase() = default;

        void Add(const SymbolResolve& sr, uint64_t func_ptr, const char* dbg_name) {
            std::lock_guard<std::mutex> lock(m_mutex);
            const void* ptr = reinterpret_cast<const void*>(func_ptr);
            if (sr.name && sr.name[0] != '\0') {
                auto [it, inserted] = m_symbols.try_emplace(sr.name, ptr);
                if (!inserted && it->second != ptr) {
                    // Round 33: detect NID conflict — same NID registered by
                    // two libraries with different implementations. Keep the
                    // first registration (ELF load order) and warn.
                    std::cerr << "[SymbolDB] WARNING: NID conflict for '"
                              << sr.name << "' — keeping first registration\n";
                }
            }
            if (dbg_name && dbg_name[0] != '\0') {
                auto [it, inserted] = m_symbols.try_emplace(dbg_name, ptr);
                if (!inserted && it->second != ptr) {
                    std::cerr << "[SymbolDB] WARNING: name conflict for '"
                              << dbg_name << "' — keeping first registration\n";
                }
            }
            m_total_count++;
        }

        void AddDirect(const std::string& name, const void* func_ptr) {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto [it, inserted] = m_symbols.try_emplace(name, func_ptr);
            if (!inserted && it->second != func_ptr) {
                std::cerr << "[SymbolDB] WARNING: direct name conflict for '"
                          << name << "' — keeping first registration\n";
            }
            m_total_count++;
        }

        const void* FindSymbol(const char* name) const {
            if (!name) return nullptr;
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_symbols.find(name);
            return (it != m_symbols.end()) ? it->second : nullptr;
        }

        // Kyty-compatible lookups.
        const SymbolRecord* FindByNid(const char* nid) const {
            if (nid == nullptr) return nullptr;
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_records.find(std::string(nid));
            return (it != m_records.end()) ? &it->second : nullptr;
        }

        const SymbolRecord* FindByNid(const char* nid, SymbolType /*type*/) const {
            return FindByNid(nid);
        }

        const SymbolRecord* FindByNid(const std::string& nid) const {
            return FindByNid(nid.c_str());
        }

        const SymbolRecord* FindByNid(const std::string& nid, SymbolType /*type*/) const {
            return FindByNid(nid.c_str());
        }

        const SymbolRecord* FindByName(const char* name) const {
            if (name == nullptr) return nullptr;
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& [key, rec] : m_records) {
                (void)key;
                if (rec.name != nullptr && std::strcmp(rec.name, name) == 0) {
                        return &rec;
                }
            }
            return nullptr;
        }

        const SymbolRecord* FindByName(const char* name, SymbolType /*type*/) const {
            return FindByName(name);
        }

        const SymbolRecord* FindByName(const std::string& name) const {
            return FindByName(name.c_str());
        }

        const SymbolRecord* FindByName(const std::string& name, SymbolType /*type*/) const {
            return FindByName(name.c_str());
        }

        void AddRecord(const SymbolRecord& rec) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_records[rec.name != nullptr ? rec.name : ""] = rec;
        }

        size_t GetCount() const { 
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_total_count; 
        }

    private:
        mutable std::mutex m_mutex;
        std::unordered_map<std::string, const void*> m_symbols;
        std::unordered_map<std::string, SymbolRecord> m_records;
        size_t m_total_count{0};
    };

}
