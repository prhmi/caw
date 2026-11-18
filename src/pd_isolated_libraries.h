#pragma once
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

// Independent copies let the existing single-instance libpd DLL keep each
// patch's global PD state isolated. Copies are private temporary files; the
// installed DLL and its dependencies are never modified.
class PdIsolatedLibraries {
public:
    ~PdIsolatedLibraries(){cleanup();}
    PdIsolatedLibraries()=default;
    PdIsolatedLibraries(const PdIsolatedLibraries&)=delete;
    PdIsolatedLibraries& operator=(const PdIsolatedLibraries&)=delete;
    bool create(const std::string& path,std::size_t count,std::string& error) {
        cleanup();std::error_code ec;
        auto source=std::filesystem::absolute(std::filesystem::u8path(path),ec);
        if(ec||!std::filesystem::is_regular_file(source,ec)||ec){error="Selected libpd library is missing.";return false;}
        if(count==0||count>256){error="PD supports 1 to 256 preloaded clips.";return false;}
        const auto sourceSize=std::filesystem::file_size(source,ec);
        if(ec||sourceSize>536870912ULL/count){error="Preloading these PD clips would exceed the 512 MB temporary-library limit.";return false;}
        auto base=std::filesystem::temp_directory_path(ec);
        if(ec){error="Temporary folder is unavailable.";return false;}
        const auto seed=std::chrono::steady_clock::now().time_since_epoch().count();
        for(int i=0;i<1000;++i) {
            folder_=base/("caw-pd-isolated-"+std::to_string(seed)+"-"+std::to_string(i));
            ec.clear();if(std::filesystem::create_directory(folder_,ec))break;
            if(ec && ec!=std::errc::file_exists){error="Cannot create private PD folder.";return false;}
            folder_.clear();
        }
        if(folder_.empty()){error="Cannot reserve a private PD folder.";return false;}
#ifdef _WIN32
        // MinGW runtime DLLs normally live next to libpd. Preserve their
        // filenames so Windows can resolve them without PATH modifications.
        std::size_t total=0, files=0;
        for(std::filesystem::directory_iterator it(source.parent_path(),ec),end; !ec&&it!=end;it.increment(ec)){
            const auto& entry=*it;
            if(entry.path()==source || entry.path().extension()!=L".dll" || !entry.is_regular_file(ec)) {ec.clear();continue;}
            const auto size=entry.file_size(ec);
            if(ec||++files>128||size>268435456ULL-total){error="PD dependencies are too large to prepare safely.";cleanup();return false;}
            total+=static_cast<std::size_t>(size);
            std::filesystem::copy_file(entry.path(),folder_/entry.path().filename(),std::filesystem::copy_options::none,ec);
            if(ec){error="Cannot copy a PD dependency.";cleanup();return false;}
        }
        if(ec){error="Cannot inspect PD dependency folder.";cleanup();return false;}
#endif
        for(std::size_t i=0;i<count;++i) {
            auto dest=folder_/std::filesystem::u8path("caw-pd-"+std::to_string(i)+source.extension().u8string());
            std::filesystem::copy_file(source,dest,std::filesystem::copy_options::none,ec);
            if(ec){error="Cannot create isolated PD library copy.";cleanup();return false;}
            libraries_.push_back(dest.u8string());
        }
        error.clear();return true;
    }
    const std::vector<std::string>& paths()const{return libraries_;}
    void cleanup() noexcept {libraries_.clear();if(!folder_.empty()){std::error_code ec;std::filesystem::remove_all(folder_,ec);folder_.clear();}}
private:
    std::filesystem::path folder_;
    std::vector<std::string> libraries_;
};
