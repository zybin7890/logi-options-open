// Drives the production provider with real Windows JSON/crypto and loopback sockets.
// Google endpoints and Credential Manager are isolated from the user's account.
#include "../config_sync_drive.cpp"
#include <iostream>

namespace config_sync {
using namespace drive;
namespace {
void expect(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
Bytes bytes(std::string_view s){return {s.begin(),s.end()};}
std::string text(std::span<const unsigned char> src){return {src.begin(),src.end()};}
struct Entry {Text id,name,created,root,next,digest,accepted;Bytes media;};
std::map<Text,Entry> entries;
std::map<Text,Text> secrets;
Text issued_challenge,issued_redirect;
unsigned issue{},serial{},refresh_count{},media_writes{};
bool revoke_token{},deny_create{},hide_latest{},race_root{};
Text rival_root;
std::string json_field(View key,View value){return "\""+utf8(key)+"\":\""+utf8(value)+"\"";}
std::string describe(const Entry& e) {
    std::string props=json_field(L"next",e.next);
    if(!e.root.empty())props+=","+json_field(L"root",e.root)+","+json_field(L"digest",e.digest)+","+json_field(L"accepted",e.accepted);
    return "{"+json_field(L"id",e.id)+","+json_field(L"name",e.name)+","+json_field(L"createdTime",e.created)+",\"appProperties\":{"+props+"}}";
}
std::map<Text,Text> query(View value) {
    std::map<Text,Text> fields;
    auto p=value.find(L'?');if(p!=value.npos)value.remove_prefix(p+1);
    while(!value.empty()) {
        auto amp=value.find(L'&');auto term=value.substr(0,amp);auto eq=term.find(L'=');
        expect(eq!=term.npos,"invalid query");fields[*decoded(utf8(term.substr(0,eq)))]=*decoded(utf8(term.substr(eq+1)));
        if(amp==value.npos)break;value.remove_prefix(amp+1);
    }
    return fields;
}
HttpReply json_reply(std::string body,unsigned status=200){return {status,{},bytes(body)};}
std::string id_json(View id){return "{"+json_field(L"id",id)+"}";}
template<class F> void fails(F&& action,std::string_view message){
    try {action();}catch(const std::exception& e){expect(std::string_view(e.what()).find(message)!=message.npos,e.what());return;}
    throw std::runtime_error("expected failure");
}
void browser(View address) {
    expect(address.starts_with(L"https://accounts.google.com/o/oauth2/v2/auth?"),"Google auth endpoint");
    auto params=query(address);
    expect(params[L"scope"]==scope&&params[L"access_type"]==L"offline"&&params[L"code_challenge_method"]==L"S256","OAuth scopes/PKCE");
    issued_challenge=params[L"code_challenge"];issued_redirect=params[L"redirect_uri"];
    expect(issued_redirect.starts_with(L"http://127.0.0.1:")&&issued_redirect.back()==L'/',"loopback redirect");
    auto host=utf8(issued_redirect.substr(7,issued_redirect.size()-8));
    auto state=params[L"state"];expect(state.size()==43,"random state");
    auto request_text="GET /?code=one%2Btime&state="+utf8(state)+" HTTP/1.1\r\nHost: "+host+"\r\n\r\n";
    expect(!callback(request_text,L"wrong",host)&&!callback(request_text,state,"not-the-host"),"reject state/Host mismatch");
    expect(!callback("GET /?code=a&state=x&state=x HTTP/1.1\r\nHost: a\r\n\r\n",L"x","a"),"reject duplicate state");
    Network wsa;Socket peer(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));sockaddr_in dest{};
    dest.sin_family=AF_INET;dest.sin_addr.s_addr=htonl(INADDR_LOOPBACK);dest.sin_port=htons(static_cast<u_short>(std::stoi(host.substr(host.find(':')+1))));
    expect(connect(peer.value,reinterpret_cast<sockaddr*>(&dest),sizeof(dest))==0,"real loopback connect");
    expect(send(peer.value,request_text.data(),static_cast<int>(request_text.size()),0)==static_cast<int>(request_text.size()),"real loopback callback");
    shutdown(peer.value,SD_SEND);
}
}

Text read_secret(View key){return secrets[Text(key)];}
void write_secret(View key,View value){secrets[Text(key)]=value;}
void erase_secret(View key){secrets.erase(Text(key));}
HttpReply request(View method,View url,View headers,std::span<const unsigned char> body,View user,View password) {
    expect(user.empty()&&password.empty(),"Unexpected Basic credentials");
    if(url==L"https://oauth2.googleapis.com/token") {
        expect(method==L"POST","token method");auto p=query(wide(text(body)));
        expect(p[L"client_id"]==L"unit.apps.googleusercontent.com"&&p[L"client_secret"]==L"test-secret","client identity");
        if(p[L"grant_type"]==L"authorization_code") {
            expect(p[L"code"]==L"one+time"&&p[L"redirect_uri"]==issued_redirect&&challenge(p[L"code_verifier"])==issued_challenge,"PKCE exchange");
            return json_reply(R"({"refresh_token":"fresh","access_token":"initial","token_type":"Bearer"})");
        }
        expect(p[L"grant_type"]==L"refresh_token"&&p[L"refresh_token"]==(refresh_count?L"rotated":L"fresh"),"refresh token rotation");
        ++refresh_count;
        return revoke_token?json_reply(R"({"error":"invalid_grant"})",400):json_reply(R"({"token_type":"Bearer","access_token":"access","refresh_token":"rotated"})");
    }
    if(url==L"https://www.googleapis.com/drive/v3/about?fields=user(permissionId)") {
        expect(method==L"GET"&&headers==L"Authorization: Bearer initial\r\n","initial Drive account check");
        return json_reply(R"({"user":{"permissionId":"account-1"}})");
    }
    expect(headers.starts_with(L"Authorization: Bearer access\r\n"),"scope token missing");
    if(url.starts_with(Text(api)+L"/generateIds?")) {
        expect(query(url)[L"space"]==L"appDataFolder","generateIds space");
        return json_reply("{\"ids\":[\"ID"+std::to_string(++issue)+"\"]}");
    }
    if(method==L"GET" && url.starts_with(Text(api)+L"?")) {
        auto p=query(url);auto q=p[L"q"];expect(p[L"spaces"]==L"appDataFolder","list outside private app folder");
        std::vector<Entry> rows;
        for(const auto& [id,e]:entries){
            if(q.find(L"name = '")!=q.npos&&e.name==anchor_name)rows.push_back(e);
            else if(!e.root.empty()&&q.find(L"value='"+e.root+L"'")!=q.npos&&!hide_latest)rows.push_back(e);
        }
        if(p[L"orderBy"]==L"name desc")std::ranges::sort(rows,std::greater{},&Entry::name);
        if(p[L"pageSize"]==L"1" && rows.size()>1)rows.resize(1);
        std::string result="{\"files\":[";
        for(std::size_t i=0;i<rows.size();++i){if(i)result+=",";result+=describe(rows[i]);}
        return json_reply(result+"]}");
    }
    if(method==L"GET"&&url.starts_with(Text(api)+L"/")) {
        auto suffix=url.substr(api.size()+1),id=suffix.substr(0,suffix.find('?'));auto it=entries.find(Text(id));
        if(it==entries.end())return {404,{},{}};
        if(suffix.ends_with(L"alt=media"))return {200,{},it->second.media};
        return json_reply(describe(it->second));
    }
    if(method==L"POST"&&url.starts_with(Text(api)+L"?")) {
        auto obj=parse(Bytes(body.begin(),body.end()));Entry e;e.id=L"ROOT"+std::to_wstring(++serial);e.name=field(obj,L"name");
        expect(e.name==anchor_name&&text(body).find("\"parents\":[\"appDataFolder\"]")!=std::string::npos,"anchor in appDataFolder");
        e.next=field(property_object(obj),L"next");e.created=std::to_wstring(serial);entries[e.id]=e;
        if(race_root){auto duplicate=e;duplicate.id=L"RIVAL";duplicate.created=L"9";duplicate.next=L"RIVAL1";entries[duplicate.id]=duplicate;rival_root=duplicate.id;}
        return json_reply(id_json(e.id));
    }
    if(method==L"POST"&&url==upload) {
        if(deny_create){deny_create=false;return {409,{},{}};}
        auto raw=text(body);auto start=raw.find("\r\n\r\n")+4;auto end=raw.find("\r\n--",start);
        auto obj=parse(bytes(raw.substr(start,end-start)));Entry e;e.id=field(obj,L"id");e.name=field(obj,L"name");
        auto props=property_object(obj);e.root=field(props,L"root");e.next=field(props,L"next");e.digest=field(props,L"digest");e.accepted=field(props,L"accepted",false);
        auto bs=raw.find("\r\n\r\n",end)+4;auto be=raw.rfind("\r\n--");e.media=bytes(raw.substr(bs,be-bs));
        expect(e.digest==hash(e.media),"byte-exact multipart upload");
        if(entries.contains(e.id))return {409,{},{}};
        entries[e.id]=e;++media_writes;
        return json_reply(id_json(e.id));
    }
    throw std::runtime_error("Unexpected Google HTTP endpoint");
}

int drive_test(){
    Apartment apt;Client client{L"unit.apps.googleusercontent.com",L"test-secret"};
    expect(challenge(L"dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")==L"E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM","RFC 7636 PKCE vector");
    fails([&]{drive::read(client);},"Connect Google Drive");
    authorize_using(client,browser);expect(read_secret(secret_key(client))==L"account-1\nfresh"&&drive::account_id(client)==L"account-1","stored grant bound to account");
    auto empty=drive::read(client);expect(!empty.exists&&entries.empty(),"read must not create a folder or file");
    race_root=true;Bytes one{0,1,13,10,255},two{8,0,9};
    write_secret(secret_key(client),L"another-account\nrotated");
    fails([&]{drive::write(client,empty,one);},"account changed");expect(entries.empty(),"account switch must not create a remote file");
    write_secret(secret_key(client),L"account-1\nrotated");
    drive::write(client,empty,one);auto first=drive::read(client);
    expect(first.exists&&!first.conflict&&first.bytes==one&&media_writes==1&&!rival_root.empty(),"racing empty anchors choose one config");
    fails([&]{drive::write(client,empty,two);},"conflict");
    deny_create=true;fails([&]{drive::write(client,first,two);},"conflict");
    hide_latest=true;drive::write(client,first,two);auto second=drive::read(client);
    expect(second.bytes==two&&second.etag!=first.etag,"stale list followed next ID");
    auto rival=entries.at(rival_root);Entry saved{rival.next,Text(file_name)+L"-0000000000000001",L"9",rival.id,L"RIVAL2",hash(one),{},one};entries[saved.id]=saved;
    auto conflict=drive::read(client);expect(conflict.conflict&&conflict.bytes==two&&conflict.etag!=second.etag,"racing first writes surface both histories");
    fails([&]{drive::write(client,second,two);},"conflict");
    drive::write(client,conflict,two);auto resolved=drive::read(client);
    expect(!resolved.conflict&&resolved.bytes==two&&entries.at(saved.id).media==one,"resolved without deleting competing bytes");
    for(auto& [id,e]:entries)if(id==resolved.etag.substr(resolved.etag.find(':')+1,resolved.etag.find_last_of(':')-resolved.etag.find(':')-1))e.media={42};
    fails([&]{drive::read(client);},"SHA-256");
    revoke_token=true;fails([&]{drive::read(client);},"expired");
    drive::disconnect(client);expect(secrets.empty(),"disconnect");
    std::cout<<"Drive OAuth/PKCE, loopback, refresh, private binary upload, integrity, races, conflict recovery and disconnect passed.\n";return 0;
}
}
int main(){try{return config_sync::drive_test();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
