// Included alongside the other Win32 settings views.
#include "native_cloud_apply.hpp"

void render_cloud(App& app) {
    const auto tr=[&](const wchar_t* zh,const wchar_t* en){return ui_settings::tr(app.ui.language,zh,en);};
    const auto& draft=app.cloud_draft;
    using enum config_sync::Provider;
    label(app,tr(L"配置同步",L"Configuration sync"),340,106,760,54,app.heading);
    constexpr int x=340,width=720;
    styled_button(app,L"OneDrive",cloud_onedrive,x,198,170,42,{Look::tab,draft.provider==onedrive,{},{},{}});
    styled_button(app,L"Google Drive",cloud_google,x+184,198,190,42,{Look::tab,draft.provider==google_drive,{},{},{}});
    styled_button(app,L"WebDAV",cloud_dav,x+388,198,160,42,{Look::tab,draft.provider==webdav,{},{},{}});
    if(draft.provider==onedrive){
        label(app,tr(L"OneDrive 同步目录",L"OneDrive sync folder"),x,284,width,28,app.bold);
        auto field=field_at(app,draft.folder.wstring(),cloud_endpoint,x,324,width,42);
        SendMessageW(field,EM_SETCUEBANNER,FALSE,reinterpret_cast<LPARAM>(tr(L"自动使用当前 OneDrive 目录",L"Use the current OneDrive folder")));
        label(app,tr(L"使用 Windows 的 OneDrive 客户端同步配置。",L"Sync through the OneDrive client on this PC."),x,382,width,36,app.small);
    } else if(draft.provider==webdav){
        label(app,tr(L"配置文件网址",L"Configuration file URL"),x,266,width,26,app.bold);
        auto field=field_at(app,draft.url,cloud_endpoint,x,297,width,38);
        SendMessageW(field,EM_SETCUEBANNER,FALSE,reinterpret_cast<LPARAM>(L"https://server.example/dav/LogiOptions.ini"));
        label(app,tr(L"用户名",L"Username"),x,353,330,24);
        label(app,tr(L"密码或应用专用密码",L"Password or app password"),x+364,353,355,24);
        field_at(app,draft.user,cloud_identity,x,383,344,38);
        field_at(app,app.cloud_secret_draft,cloud_password,x+364,383,356,38,ES_AUTOHSCROLL|ES_PASSWORD);
    } else {
        label(app,tr(L"桌面应用客户端 ID",L"Desktop OAuth client ID"),x,266,width,24,app.bold);
        field_at(app,draft.client_id,cloud_identity,x,298,width,38);
        label(app,tr(L"客户端密钥",L"Client secret"),x,348,width,24);
        field_at(app,app.cloud_secret_draft,cloud_password,x,378,width,38,ES_AUTOHSCROLL|ES_PASSWORD);
        styled_button(app,tr(L"连接 Google Drive",L"Connect Google Drive"),cloud_authorize,x,436,218,37,{Look::plain,false,{},{},{}});
        styled_button(app,tr(L"断开连接",L"Disconnect"),cloud_disconnect,x+232,436,142,37,{Look::plain,false,{},{},{}});
    }
    toggle_at(app,tr(L"每分钟自动同步",L"Sync automatically every minute"),draft.automatic,cloud_automatic,x,504,width,true);
    button(app,tr(L"立即同步",L"Sync now"),cloud_run,x,568,170,42,true);
    styled_button(app,tr(L"保存连接设置",L"Save connection"),cloud_save,x+184,568,190,42,{Look::plain,false,{},{},{}});
    if(!app.cloud_status.empty())label(app,app.cloud_status,x,638,970,57,app.small);
    if(app.cloud_conflict){
        styled_button(app,tr(L"使用本机版本",L"Use this PC's copy"),cloud_local,x,706,210,39,{Look::plain,false,{},{},{}});
        styled_button(app,tr(L"使用云端版本",L"Use the remote copy"),cloud_remote,x+224,706,210,39,{Look::plain,false,{},{},{}});
    }
    if(app.cloud_job || app.cloud_queued){
        for(int id=cloud_dav;id<=cloud_disconnect;++id)
            if(const auto control=GetDlgItem(app.window,id))EnableWindow(control,FALSE);
    }
}
void update_cloud_draft(App& app,int id,HWND control){
    auto text=control_text(control);
    if(id==cloud_password)app.cloud_secret_draft=std::move(text);
    else if(id==cloud_endpoint){
        if(app.cloud_draft.provider==config_sync::Provider::onedrive)app.cloud_draft.folder=text;
        else app.cloud_draft.url=std::move(text);
    } else if (id==cloud_identity){
        if(app.cloud_draft.provider==config_sync::Provider::google_drive)app.cloud_draft.client_id=std::move(text);
        else app.cloud_draft.user=std::move(text);
    }
}
void save_cloud_options(App& app){
    const auto& d=app.cloud_draft;
    if(d.provider==config_sync::Provider::webdav && d.url.empty())throw std::runtime_error("Enter the WebDAV file URL");
    if(d.provider==config_sync::Provider::google_drive && d.client_id.empty())throw std::runtime_error("Enter a Google desktop OAuth client ID");
    if(d.provider==config_sync::Provider::onedrive){
        const auto folder=d.folder.empty()?config_sync::onedrive_folder():d.folder;
        if(!folder.is_absolute() || !fs::is_directory(folder))throw std::runtime_error("Choose an existing OneDrive folder");
    }
    if(!app.cloud_secret_draft.empty() && d.provider!=config_sync::Provider::onedrive){
        config_sync::write_secret(config_sync::credential_key(d),app.cloud_secret_draft);
        SecureZeroMemory(app.cloud_secret_draft.data(),app.cloud_secret_draft.size()*sizeof(wchar_t));
        app.cloud_secret_draft.clear();
    }
    config_sync::save_options(app.mapping(),d);
    app.cloud_options=d;
    if(d.automatic)SetTimer(app.window,32,60000,nullptr);
    else KillTimer(app.window,32);
}
void start_cloud_job(App& app,config_sync::Mode mode,bool auth,bool automatic){
    if(app.cloud_job || (automatic && app.cloud_conflict))return;
    if(!auth && cloud_busy(app)){
        if(!automatic){
            app.cloud_queued=mode;app.cloud_queue_deadline=GetTickCount64()+30000;
            app.cloud_status=L"正在等待本地设置保存，随后会自动同步。";
            SetTimer(app.window,34,200,nullptr);
        }
        return;
    }
    auto job=std::make_shared<CloudJob>();
    job->options=app.cloud_options;
    job->authorization=auth;
    const auto path=app.mapping();
    if(!auth)job->local=cloud_snapshot(app);
    std::thread task([job,path,mode]{
        try {
            if(job->authorization){
                config_sync::drive::authorize({job->options.client_id,config_sync::read_secret(config_sync::credential_key(job->options))});
            } else {
                job->result=config_sync::synchronize(path,job->options,job->local,mode);
                if(job->options.provider==config_sync::Provider::onedrive &&
                   (job->result.status==config_sync::Status::uploaded || job->result.status==config_sync::Status::unchanged)){
                    const auto folder=job->options.folder.empty()?config_sync::onedrive_folder():job->options.folder;
                    job->cloud_confirmed=config_sync::onedrive_in_sync(folder,job->local);
                }
            }
        } catch(const std::exception& e){job->error=from_utf8(e.what());}
        job->finished.store(true,std::memory_order_release);
    });
    task.detach();
    app.cloud_job=std::move(job);app.cloud_applying=false;app.cloud_deadline=0;
    SetTimer(app.window,33,200,nullptr);
    app.cloud_status=auth?L"等待浏览器授权…":L"正在同步…";
}
void click_cloud(App& app,int id){
    using enum config_sync::Provider;
    try {
        if(id==cloud_settings){
            app.cloud_draft=app.cloud_options;app.cloud_secret_draft.clear();
            app.cloud_open=true;render(app);return;
        }
        if(app.cloud_job || app.cloud_queued)return;
        if(id==cloud_dav || id==cloud_google || id==cloud_onedrive){
            app.cloud_draft.provider=id==cloud_dav?webdav:id==cloud_google?google_drive:onedrive;
            app.cloud_secret_draft.clear();app.cloud_conflict=false;app.cloud_status.clear();
        } else if(id==cloud_disconnect){
            config_sync::drive::disconnect({app.cloud_draft.client_id,{}});
            app.cloud_draft.automatic=false;save_cloud_options(app);
            app.cloud_status=L"已断开本机的 Google Drive 授权。";
        } else {
            if(id==cloud_automatic)app.cloud_draft.automatic=!app.cloud_draft.automatic;
            save_cloud_options(app);
            if(id==cloud_automatic || id==cloud_save)app.cloud_status=L"连接设置已保存。";
            else start_cloud_job(app,id==cloud_local?config_sync::Mode::prefer_local:id==cloud_remote?config_sync::Mode::prefer_remote:config_sync::Mode::sync,id==cloud_authorize,false);
        }
    } catch(const std::exception& e){app.cloud_status=from_utf8(e.what());}
    render(app);
}
void cloud_retry(App& app){
    if(app.cloud_queued && cloud_busy(app) && GetTickCount64()<app.cloud_queue_deadline)return;
    const auto pending=app.cloud_queued;app.cloud_queued.reset();KillTimer(app.window,34);
    if(!pending)return;
    try {
        if(cloud_busy(app))throw std::runtime_error("Local settings are still busy; sync did not start");
        start_cloud_job(app,*pending,false,false);
    } catch(const std::exception& e){app.cloud_status=from_utf8(e.what());}
    if(app.page==settings && app.cloud_open)render(app);
}
void cloud_timer(App& app,bool periodic){
    try {
        if(periodic){
            if(app.cloud_options.automatic && !app.cloud_queued)start_cloud_job(app,config_sync::Mode::sync,false,true);
            return;
        }
        if(!app.cloud_job || !app.cloud_job->finished.load(std::memory_order_acquire))return;
        auto& job=*app.cloud_job;
        if(!job.error.empty())app.cloud_status=job.error;
        else if(job.authorization)app.cloud_status=L"Google Drive 已连接。";
        else {
            using enum config_sync::Status;
            app.cloud_conflict=job.result.status==conflict;
            switch(job.result.status){
            case unchanged:app.cloud_status=job.options.provider!=config_sync::Provider::onedrive?L"本机与云端配置一致。":
                job.cloud_confirmed?L"OneDrive 已确认该文件同步完成。":L"配置与 OneDrive 本地目录一致，尚无云端确认。";break;
            case uploaded:app.cloud_status=job.options.provider!=config_sync::Provider::onedrive?L"配置已上传。":
                job.cloud_confirmed?L"OneDrive 已确认该文件同步完成。":L"配置已写入 OneDrive 本地目录，尚无云端确认。";break;
            case conflict:app.cloud_status=L"两个版本不同。请选择要保留的版本。";break;
            case download:
                if(!app.cloud_deadline)app.cloud_deadline=GetTickCount64()+30000;
                if(GetTickCount64()>app.cloud_deadline)throw std::runtime_error("Timed out confirming local configuration");
                if(apply_cloud_download(app,job)==ApplyProgress::waiting)return;
                config_sync::acknowledge_download(app.mapping(),job.options,job.result,cloud_snapshot(app));
                app.cloud_status=L"远端配置已在本机生效。";break;
            }
        }
    } catch(const std::exception& e){app.cloud_status=from_utf8(e.what());if(periodic)return;}
    app.cloud_job.reset();app.cloud_applying=false;
    KillTimer(app.window,33);
    if(app.page==settings && app.cloud_open)render(app);
}
