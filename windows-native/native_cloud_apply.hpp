// Included inside native.cpp's anonymous namespace, beside the device and reload paths.
std::optional<std::string> read_cloud_input(const fs::path& path,std::size_t limit) {
    if(!fs::exists(path))return {};
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("Cannot read saved configuration");
    const auto size=input.tellg();
    if(size<0 || size>static_cast<std::streamoff>(limit))
        throw std::runtime_error("Invalid or oversized saved configuration");
    std::string bytes(static_cast<std::size_t>(size),'\0');
    input.seekg(0);
    if(!input.read(bytes.data(),static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Saved configuration changed during read");
    return bytes;
}

std::string cloud_line(std::istringstream& stream) {
    std::string value;
    if(!std::getline(stream,value))throw std::runtime_error("Incomplete saved mouse settings");
    if(value.ends_with('\r'))value.pop_back();
    if(value.empty())throw std::runtime_error("Empty saved mouse identifier");
    return value;
}

std::vector<unsigned char> cloud_snapshot(const App& app) {
    const auto mapping=app.mapping();
    const auto source=read_cloud_input(mapping,65538);
    if(!source)throw std::runtime_error("Active mappings file is missing");
    const auto mappings=to_utf8(from_utf8(*source)); // Native loader accepts UTF-8, including a leading BOM.
    if(mappings.empty())throw std::runtime_error("Invalid or empty UTF-8 mappings file");
    loo_backup::Configuration config{mappings};
    auto dpi_path=mapping,settings_path=mapping;
    const auto dpi=read_cloud_input(dpi_path.replace_extension(L".dpi"),4096);
    const auto settings=read_cloud_input(settings_path.replace_extension(L".settings"),16384);
    std::string device;
    if(dpi) {
        std::istringstream input(*dpi);
        device=cloud_line(input);
        unsigned value{};std::string extra;
        if(!(input>>value) || input>>extra)throw std::runtime_error("Invalid saved DPI");
        config.settings[0]=value;
    }
    if(settings) {
        std::istringstream input(*settings);
        if(cloud_line(input)!="LogiNative settings 1")throw std::runtime_error("Unsupported saved settings version");
        const auto name=cloud_line(input);
        if(!device.empty() && device!=name)throw std::runtime_error("Saved settings refer to different devices");
        std::string key;
        while(input>>key) {
            const auto found=std::find(loo_backup::names.begin()+1,loo_backup::names.end(),key);
            unsigned value{};
            if(found==loo_backup::names.end() || !(input>>value))throw std::runtime_error("Invalid saved wheel settings");
            const auto index=static_cast<unsigned>(found-loo_backup::names.begin());
            if(config.settings[index])throw std::runtime_error("Duplicate saved wheel setting");
            config.settings[index]=value;
        }
    }
    const auto package=loo_backup::encode(config);
    return {package.begin(),package.end()};
}

bool cloud_busy(const App& app) {
    return app.busy || app.reload_busy || app.pending_reload || !app.pending_values.empty() ||
           app.pending_binding || app.pending_ring || app.pending_thumb_speed || app.pending_sensitivity ||
           app.ring_dragging || app.ring_slot_dragging || app.waiting_agent || app.waiting_stop || app.closing ||
           (app.window && !IsWindowEnabled(app.window));
}

void save_cloud_recovery(const App& app,std::span<const unsigned char> before) {
    const auto directory=app.directory/L"sync-backups";
    fs::create_directories(directory);
    FILETIME ft{};GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER filetime{};filetime.HighPart=ft.dwHighDateTime;filetime.LowPart=ft.dwLowDateTime;
    const auto stem=L"before-cloud-"+std::to_wstring(filetime.QuadPart)+L"-"+std::to_wstring(GetCurrentProcessId());
    fs::path path;
    HANDLE file;
    for(unsigned long long suffix=0;;++suffix){
        const auto name=suffix?stem+L"-"+std::to_wstring(suffix):stem;
        path=directory/(name+L".ini");
        file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file!=INVALID_HANDLE_VALUE)break;
        const auto error=GetLastError();
        if(error!=ERROR_FILE_EXISTS && error!=ERROR_ALREADY_EXISTS)
            throw std::runtime_error("Cannot create local recovery backup (Windows error "+std::to_string(error)+")");
    }
    DWORD written{};
    const bool valid=WriteFile(file,before.data(),static_cast<DWORD>(before.size()),&written,nullptr) &&
        written==before.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if(!valid){DeleteFileW(path.c_str());throw std::runtime_error("Failed to save local recovery backup");}
}

enum class ApplyProgress { waiting, done };
ApplyProgress apply_cloud_download(App& app,CloudJob& job) {
    using Stage=CloudJob::ApplyStage;
    if(cloud_busy(app))return ApplyProgress::waiting;
    const auto current=read_agent_status();
    if(!current.available || !current.mappings_ready || options_running())
        throw std::runtime_error("Connect the mouse and start the local agent before applying a download");
    if(job.apply_stage==Stage::prepare || job.apply_stage==Stage::probe) {
        if(cloud_snapshot(app)!=job.local)throw std::runtime_error("Local settings changed during sync; download was not applied");
        if(job.apply_stage==Stage::prepare) {
            job.download=loo_backup::decode({reinterpret_cast<const char*>(job.result.package.data()),job.result.package.size()});
            const auto text=from_utf8(job.download.mappings);
            if(text.empty() || to_utf8(text)!=job.download.mappings)throw std::runtime_error("Invalid UTF-8 in downloaded mappings");
            if(std::ranges::any_of(job.download.settings,[](const auto& value){return value.has_value();})) {
                job.apply_stage=Stage::probe;
                io(app); // Asynchronous, also when only the tray window exists.
                return ApplyProgress::waiting;
            }
        }
        for(unsigned id=0;id<job.download.settings.size();++id)
            if(job.download.settings[id] && !app.device.values[id])
                throw std::runtime_error("Cannot verify a requested setting on this mouse");
        if(job.download.settings[0] && !std::ranges::contains(app.device.dpi,*job.download.settings[0]))
            throw std::runtime_error("Downloaded DPI is unsupported by this mouse");
        read_config(app);
        const auto before=loo_backup::decode({reinterpret_cast<const char*>(job.local.data()),job.local.size()});
        if(to_utf8(app.source)!=before.mappings)throw std::runtime_error("Local mappings changed before apply");
        save_cloud_recovery(app,job.local);
        // save() validates a temporary file with the real mapping parser before
        // atomically replacing the active file and submitting the reload.
        if(!save(app,from_utf8(job.download.mappings)))throw std::runtime_error("Downloaded mappings were not applied; see sync-backups");
        job.apply_reload_revision=app.reload_revision;
        job.apply_dpi_revision=app.dpi_revision;
        job.apply_setting_revision=app.setting_revision;
        job.apply_stage=Stage::reload;
        return ApplyProgress::waiting;
    }

    if(app.reload_revision!=job.apply_reload_revision || app.dpi_revision!=job.apply_dpi_revision ||
       app.setting_revision!=job.apply_setting_revision)
        throw std::runtime_error("A new local edit superseded this download; see sync-backups");
    if(!app.last_reload_result || app.last_reload_result->revision!=job.apply_reload_revision ||
       app.last_reload_result->status!=ReloadStatus::applied || !is_current_reload_result(*app.last_reload_result))
        throw std::runtime_error("Agent did not confirm the downloaded mappings; see sync-backups");
    if(to_utf8(read(app.mapping()))!=job.download.mappings)
        throw std::runtime_error("Mappings changed during local commit; see sync-backups");
    if(job.apply_stage==Stage::reload) {
        const auto before=loo_backup::decode({reinterpret_cast<const char*>(job.local.data()),job.local.size()});
        const auto snapshot=cloud_snapshot(app);
        const auto after=loo_backup::decode({reinterpret_cast<const char*>(snapshot.data()),snapshot.size()});
        if(before.settings!=after.settings)throw std::runtime_error("Mouse settings changed while mappings reloaded; see sync-backups");
        for(unsigned id=0;id<job.download.settings.size();++id)if(const auto value=job.download.settings[id]) {
            // A matching cached DPI still needs this request's readback/persistence ACK.
            if(id==0)app.dpi_unconfirmed=true;
            queue_parameter(app,id,*value,true);
        }
        job.apply_dpi_revision=app.dpi_revision;
        job.apply_setting_revision=app.setting_revision;
        job.apply_stage=Stage::settings;
        flush_parameters(app);
        return ApplyProgress::waiting;
    }
    const auto snapshot=cloud_snapshot(app);
    const auto saved=loo_backup::decode({reinterpret_cast<const char*>(snapshot.data()),snapshot.size()});
    for(unsigned id=0;id<job.download.settings.size();++id)if(const auto value=job.download.settings[id]) {
        if((id==0?app.dpi_unconfirmed:app.setting_unconfirmed[id]) ||
           app.device.values[id]!=value || saved.settings[id]!=value)
            throw std::runtime_error("Mouse readback or saved settings differ from the download; see sync-backups");
    }
    return ApplyProgress::done;
}
