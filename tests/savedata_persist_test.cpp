// ============================================================================
// ProsperoLayer RDNA2 Core - save-data persistence test (round 28, HLE layer)
// ----------------------------------------------------------------------------
// Proves the libSaveData surface now round-trips REAL data through the host
// filesystem (before round 28 SetParam/GetParam/SaveIcon/LoadIcon were
// logging no-ops):
//
//   * SaveDataMount3 (create) builds the real directory and mounts it at
//     /savedata0 through FileSystem::Mount;
//   * SaveDataSetParam writes a PARAM.bin container INSIDE the save dir and
//     SaveDataGetParam (after umount + remount!) reads the same struct back
//     -- persistence across mounts, not just in-memory state;
//   * SaveDataSaveIcon persists the raw icon bytes to icon0.png and
//     SaveDataLoadIcon reads them back (bounded by the guest buffer);
//   * DirNameSearch lists the created save directory;
//   * SaveDataDelete removes the directory from the host filesystem;
//   * the guest path /savedata0/... resolves to the same host directory the
//     mount created (GetRealFilename).
// ============================================================================
#include "kernel/fileSystem.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

// Guest-ABI mirrors declared INSIDE Libs::SaveData so the extern function
// declarations mangle identically to libs/libSaveData.cpp (same namespace,
// same standard-layout definitions).
namespace Libs::SaveData {

struct SceSaveDataDirName {
    char data[32];
};
struct SceSaveDataTitleId {
    char data[10];
    char padding[6];
};
struct SaveDataMountPoint {
    char data[16];
};
struct SaveDataMount3 {
    int                       user_id;
    int                       pad;
    const SceSaveDataDirName* dir_name;
    uint64_t                  blocks;
    uint64_t                  system_blocks;
    uint32_t                  mount_mode;
    int                       pad2;
    int32_t                   resource;
    uint8_t                   reserved[32];
};
struct SaveDataMountResult {
    SaveDataMountPoint mount_point;
    uint64_t           required_blocks;
    uint32_t           unused;
    uint32_t           mount_status;
    uint8_t            reserved[28];
    int                pad;
};
struct SaveDataParam {
    char     title[128];
    char     sub_title[128];
    char     detail[1024];
    uint32_t user_param;
    int      pad;
    int64_t  mtime;
    uint8_t  reserved[32];
};
struct SaveDataSearchInfo {
    uint64_t blocks;
    uint64_t free_blocks;
    uint8_t  reserved[32];
};
struct SaveDataDirNameSearchCond {
    int32_t                   user_id;
    int32_t                   pad;
    const void*               title_id;
    const SceSaveDataDirName* dir_name;
    uint32_t                  key;
    uint32_t                  order;
    uint8_t                   reserved[32];
};
struct SaveDataDirNameSearchResult {
    uint32_t            hit_num;
    int32_t             pad;
    SceSaveDataDirName* dir_names;
    uint32_t            dir_names_num;
    uint32_t            set_num;
    SaveDataParam*      params;
    SaveDataSearchInfo* infos;
    uint8_t             reserved[12];
    int32_t             pad2;
};
struct SaveDataIcon {
    void*   buf;
    size_t  buf_size;
    size_t  data_size;
    uint8_t reserved[32];
};

int KYTY_SYSV_ABI SaveDataMount3(const SaveDataMount3* mount,
                                 SaveDataMountResult* mount_result);
int KYTY_SYSV_ABI SaveDataUmount2(uint32_t mode,
                                  const SaveDataMountPoint* mount_point);
int KYTY_SYSV_ABI SaveDataSetParam(const SaveDataMountPoint* mount_point,
                                   uint32_t param_type, const void* param_buf,
                                   size_t param_buf_size);
int KYTY_SYSV_ABI SaveDataGetParam(const SaveDataMountPoint* mount_point,
                                   uint32_t param_type, void* param_buf,
                                   size_t param_buf_size, size_t* got_size);
int KYTY_SYSV_ABI SaveDataSaveIcon(const SaveDataMountPoint* mount_point,
                                   const SaveDataIcon* icon);
int KYTY_SYSV_ABI SaveDataLoadIcon(const SaveDataMountPoint* mount_point,
                                   SaveDataIcon* icon);
int KYTY_SYSV_ABI SaveDataDirNameSearch(const SaveDataDirNameSearchCond* cond,
                                        SaveDataDirNameSearchResult* result);
struct SaveDataDelete;   // forward (the definition follows the function)
int KYTY_SYSV_ABI SaveDataDelete(const SaveDataDelete* del);
struct SaveDataDelete {
    int32_t                        user_id;
    int32_t                        pad;
    const SceSaveDataTitleId*      title_id;
    const SceSaveDataDirName*      dir_name;
    uint32_t                       unused;
    uint8_t                        reserved[32];
    int32_t                        pad2;
};

} // namespace Libs::SaveData

namespace {
using namespace Libs;
using Mount3Req      = struct Libs::SaveData::SaveDataMount3;
using MountPoint     = Libs::SaveData::SaveDataMountPoint;
using MountResult    = Libs::SaveData::SaveDataMountResult;
using SaveParam      = Libs::SaveData::SaveDataParam;
using IconBuf        = Libs::SaveData::SaveDataIcon;
using SearchCond     = Libs::SaveData::SaveDataDirNameSearchCond;
using SearchRes      = Libs::SaveData::SaveDataDirNameSearchResult;
using SearchInfo     = Libs::SaveData::SaveDataSearchInfo;
using DirName        = Libs::SaveData::SceSaveDataDirName;
int g_failures = 0, g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)
} // namespace

int main() {
    // The save root lives under _SaveData/<title_id>/ -- point the CWD at a
    // scratch directory so the test never touches a real install.
    const std::string scratch = "/tmp/prospero_savedata_test_XXXXXX";
    std::vector<char> dir(scratch.begin(), scratch.end());
    dir.push_back('\0');
    const char* made = ::mkdtemp(dir.data());
    CHECK(made != nullptr);
    CHECK(::chdir(made) == 0);
    std::printf("[savedata] scratch dir: %s\n", made);

    std::vector<uint8_t> png(256);
    for (size_t i = 0; i < png.size(); ++i) png[i] = static_cast<uint8_t>(i);

    std::printf("[savedata] A: mount(create) -> real directory + mount point\n");
    Mount3Req mount{};
    DirName dirname{};
    std::snprintf(dirname.data, sizeof(dirname.data), "ROUND28_SAVE");
    mount.user_id    = 1;
    mount.dir_name   = &dirname;
    mount.blocks     = 1024;
    mount.mount_mode = 4;      // create
    MountResult result{};
    CHECK(SaveData::SaveDataMount3(&mount, &result) == 0);
    CHECK(std::string(result.mount_point.data) == "/savedata0");
    CHECK(result.mount_status == 1);              // created

    // the mounted guest path resolves to the real host directory
    const std::string real =
        LibKernel::FileSystem::GetRealFilename("/savedata0/PARAM.bin");
    CHECK(real.find("_SaveData") != std::string::npos);
    CHECK(real.find("ROUND28_SAVE") != std::string::npos);
    CHECK(real.find("PARAM.bin") != std::string::npos);

    std::printf("[savedata] B: SetParam -> real PARAM.bin\n");
    {
        SaveParam param{};
        std::snprintf(param.title, sizeof(param.title), "Round 28 Test Save");
        std::snprintf(param.sub_title, sizeof(param.sub_title), "persistence");
        std::snprintf(param.detail, sizeof(param.detail),
                      "saved through the real host filesystem");
        param.user_param = 0xC0FFEEu;
        param.mtime      = 1735689600;
        CHECK(SaveDataSetParam(&result.mount_point, 0, &param,
                                         sizeof(param)) == 0);
        // the file exists on the host with the documented container header
        FILE* f = std::fopen(real.c_str(), "rb");
        CHECK(f != nullptr);
        if (f != nullptr) {
            uint32_t header[3] = {};
            CHECK(std::fread(header, 4, 3, f) == 3);
            CHECK(header[0] == 0x44534C50u);      // "PLSD"
            CHECK(header[1] == 1u);               // version
            CHECK(header[2] == sizeof(SaveParam));
            std::fclose(f);
        }
    }

    std::printf("[savedata] C: SaveIcon -> real icon0.png\n");
    {
        IconBuf icon{};
        icon.buf       = png.data();
        icon.buf_size  = png.size();
        icon.data_size = 128;                     // half the buffer
        CHECK(SaveDataSaveIcon(&result.mount_point, &icon) == 0);
        // the raw bytes are on the host
        const std::string icon_path =
            LibKernel::FileSystem::GetRealFilename("/savedata0/icon0.png");
        FILE* f = std::fopen(icon_path.c_str(), "rb");
        CHECK(f != nullptr);
        if (f != nullptr) {
            uint8_t back[128] = {};
            CHECK(std::fread(back, 1, 128, f) == 128);
            CHECK(std::memcmp(back, png.data(), 128) == 0);
            std::fclose(f);
        }
        // fail-closed: data_size > buf_size
        IconBuf bad{};
        bad.buf = png.data(); bad.buf_size = 8; bad.data_size = 128;
        CHECK(SaveDataSaveIcon(&result.mount_point, &bad) != 0);
    }

    std::printf("[savedata] D: umount -> remount -> GetParam/LoadIcon read back\n");
    {
        CHECK(SaveDataUmount2(0, &result.mount_point) == 0);
        // remount in OPEN mode (mount_mode 3) over the existing directory
        Mount3Req reopen{};
        reopen.user_id    = 1;
        reopen.dir_name   = &dirname;
        reopen.mount_mode = 3;      // open
        MountResult result2{};
        CHECK(SaveData::SaveDataMount3(&reopen, &result2) == 0);
        CHECK(result2.mount_status == 0);          // not created
        CHECK(std::string(result2.mount_point.data) == "/savedata0");

        // the param SURVIVED the unmount cycle: read it back
        SaveParam back{};
        size_t got = 0;
        CHECK(SaveDataGetParam(&result2.mount_point, 0, &back,
                                         sizeof(back), &got) == 0);
        CHECK(got == sizeof(SaveParam));
        CHECK(std::string(back.title) == "Round 28 Test Save");
        CHECK(std::string(back.sub_title) == "persistence");
        CHECK(std::string(back.detail) ==
              "saved through the real host filesystem");
        CHECK(back.user_param == 0xC0FFEEu);
        CHECK(back.mtime == 1735689600);

        // the icon read is bounded by the guest buffer
        std::vector<uint8_t> icon_buf(64);
        IconBuf icon{};
        icon.buf      = icon_buf.data();
        icon.buf_size = icon_buf.size();
        CHECK(SaveDataLoadIcon(&result2.mount_point, &icon) == 0);
        CHECK(icon.data_size == 64);               // clamped to buf_size
        CHECK(std::memcmp(icon_buf.data(), png.data(), 64) == 0);

        // a fresh save without PARAM.bin reads back zeroed (not an error)
        CHECK(SaveDataUmount2(0, &result2.mount_point) == 0);
    }

    std::printf("[savedata] E: DirNameSearch lists the save\n");
    {
        SearchCond cond{};
        cond.user_id = 1;
        DirName names[8]{};
        SaveParam params[8]{};
        SearchInfo infos[8]{};
        SearchRes res{};
        res.dir_names    = names;
        res.dir_names_num = 8;
        res.params       = params;
        res.infos        = infos;
        CHECK(SaveDataDirNameSearch(&cond, &res) == 0);
        CHECK(res.hit_num >= 1);
        bool found = false;
        for (uint32_t i = 0; i < res.set_num; ++i) {
            if (std::string(names[i].data) == "ROUND28_SAVE") found = true;
        }
        CHECK(found);
    }

    std::printf("[savedata] F: Delete removes the host directory\n");
    {
        struct SaveData::SaveDataDelete del{};
        del.user_id  = 1;
        del.dir_name = &dirname;
        CHECK(SaveData::SaveDataDelete(&del) == 0);
        // the directory is gone: reopen in open mode now fails NOT_FOUND
        Mount3Req reopen{};
        reopen.user_id    = 1;
        reopen.dir_name   = &dirname;
        reopen.mount_mode = 3;
        MountResult result3{};
        CHECK(SaveData::SaveDataMount3(&reopen, &result3) != 0);
    }

    std::printf("[savedata] %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
