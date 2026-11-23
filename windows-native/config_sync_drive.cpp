#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <roapi.h>
#include <shellapi.h>
#include <winstring.h>
#ifdef __MINGW32__
// This SDK aliases both WinRT boolean and BYTE to unsigned char and emits
// duplicate IReference specializations. JSON only uses the raw boolean ABI.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#endif
#include <windows.data.json.h>
#include <wrl/client.h>
#include "config_sync_drive.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace config_sync::drive {
namespace {
using Bytes=std::vector<unsigned char>;
using Text=std::wstring;
using View=std::wstring_view;
namespace json=ABI::Windows::Data::Json;
template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
using Object=Ptr<json::IJsonObject>;
using Array=Ptr<json::IJsonArray>;
constexpr View scope=L"https://www.googleapis.com/auth/drive.appdata";
constexpr View api=L"https://www.googleapis.com/drive/v3/files";
constexpr View upload=L"https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart&fields=id";
constexpr View anchor_name=L"logi-options-open-anchor-v1";
constexpr View file_name=L"logi-options-open-config-v1";
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Invalid JSON returned by Google Drive.");}

struct Apartment {
    HRESULT hr=RoInitialize(RO_INIT_MULTITHREADED);
    Apartment(){require(SUCCEEDED(hr)||hr==RPC_E_CHANGED_MODE,"Could not initialize Windows JSON runtime.");}
    ~Apartment(){if(SUCCEEDED(hr))RoUninitialize();}
};
struct HString {
    HSTRING h{};
    HString()=default;
    explicit HString(View s){check(WindowsCreateString(s.data(),static_cast<UINT32>(s.size()),&h));}
    HString(const HString&)=delete;
    ~HString(){WindowsDeleteString(h);}
    Text value() const {UINT32 n{};auto p=WindowsGetStringRawBuffer(h,&n);return p?Text(p,n):Text{};}
};
Text wide(std::string_view source) {
    if(source.empty())return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,source.data(),static_cast<int>(source.size()),nullptr,0);
    require(n>0,"Invalid UTF-8 in Drive response.");Text out(n,L' ');
    require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,source.data(),static_cast<int>(source.size()),out.data(),n)==n,"Invalid UTF-8 in Drive response.");return out;
}
std::string utf8(View source) {
    if(source.empty())return {};
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,source.data(),static_cast<int>(source.size()),nullptr,0,nullptr,nullptr);
    require(n>0,"Invalid UTF-16 value.");std::string out(n,' ');
    require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,source.data(),static_cast<int>(source.size()),out.data(),n,nullptr,nullptr)==n,"Invalid UTF-16 value.");return out;
}
std::span<const unsigned char> byte_view(std::string_view text){
    return {reinterpret_cast<const unsigned char*>(text.data()),text.size()};
}
Object parse(const Bytes& bytes) {
    HString name(L"Windows.Data.Json.JsonValue");Ptr<json::IJsonValueStatics> factory;
    check(RoGetActivationFactory(name.h,__uuidof(json::IJsonValueStatics),reinterpret_cast<void**>(factory.GetAddressOf())));
    HString source(wide({reinterpret_cast<const char*>(bytes.data()),bytes.size()}));
    Ptr<json::IJsonValue> value;check(factory->Parse(source.h,&value));Object object;check(value->GetObject(&object));return object;
}
Text field(const Object& obj,View key,bool required=true) {
    HString k(key),value;const auto hr=obj->GetNamedString(k.h,&value.h);
    if(FAILED(hr)&&!required)return {};
    check(hr);auto text=value.value();require(!required||!text.empty(),"Google omitted a required value.");return text;
}
Array array(const Object& obj,View key) {
    HString k(key);Array value;check(obj->GetNamedArray(k.h,&value));return value;
}
Object property_object(const Object& obj) {
    HString k(L"appProperties");Object value;check(obj->GetNamedObject(k.h,&value));return value;
}
bool alpha(unsigned c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');}
Text encoded(View value) {
    constexpr wchar_t hex[]=L"0123456789ABCDEF";Text out;
    for(unsigned char c:utf8(value)) {
        if(alpha(c)||c=='-'||c=='.'||c=='_'||c=='~')out+=static_cast<wchar_t>(c);
        else {out+=L'%';out+=hex[c>>4];out+=hex[c&15];}
    }return out;
}
bool valid_id(View id) {
    return !id.empty()&&id.size()<=160&&std::ranges::all_of(id,[](wchar_t c){return alpha(c)||c==L'-'||c==L'_';});
}
Text secret_key(const Client& client) {
    require(client.id.ends_with(L".apps.googleusercontent.com")&&client.id.size()<=256&&
        std::ranges::all_of(client.id,[](wchar_t c){return alpha(c)||c==L'.'||c==L'-'||c==L'_';}),
        "Configure a Google Desktop app OAuth client ID first.");
    return L"drive-refresh/"+client.id;
}
struct GrantLock {
    HANDLE handle{};
    explicit GrantLock(const Client& client) {
        handle=CreateMutexW(nullptr,FALSE,(L"Local\\LogiOptionsOpen.DriveGrant."+client.id).c_str());
        require(handle!=nullptr,"Cannot lock Google Drive credentials.");
        const auto wait=WaitForSingleObject(handle,30000);
        if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED) {
            CloseHandle(handle);throw std::runtime_error("Another Google Drive authorization is busy.");
        }
    }
    ~GrantLock(){ReleaseMutex(handle);CloseHandle(handle);}
    GrantLock(const GrantLock&)=delete;
};
Text b64(std::span<const unsigned char> bytes) {
    constexpr wchar_t alphabet[]=L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    Text result;unsigned bits{},acc{};
    for(unsigned char b:bytes){acc=(acc<<8)|b;bits+=8;while(bits>=6){bits-=6;result+=alphabet[(acc>>bits)&63];}}
    if(bits)result+=alphabet[(acc<<(6-bits))&63];
    return result;
}
Text random_code() {
    std::array<unsigned char,32> bytes{};
    require(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"Windows could not create a secure random value.");
    return b64(bytes);
}
Text hash(std::span<const unsigned char> bytes);
Text challenge(View verifier) {return hash(byte_view(utf8(verifier)));}
int nibble(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
std::optional<Text> decoded(std::string_view v) {
    std::string s;for(std::size_t i=0;i<v.size();++i){
        auto c=v[i];if(c=='%'){if(i+2>=v.size())return {};int a=nibble(v[i+1]),b=nibble(v[i+2]);if(a<0||b<0)return {};c=static_cast<char>((a<<4)|b);i+=2;}
        else if(c=='+')c=' ';
        if(static_cast<unsigned char>(c)<32||c==127)return {};
        s+=c;
    }
    try{return wide(s);}catch(const std::runtime_error&){return {};}
}
struct Callback {Text code;bool denied{};};
std::optional<Callback> callback(std::string_view request_text,View state,std::string_view host) {
    const auto end=request_text.find("\r\n");if(end==request_text.npos)return {};
    auto line=request_text.substr(0,end);
    if(!line.starts_with("GET /?")||!line.ends_with(" HTTP/1.1"))return {};
    auto headers=request_text.substr(end+2);bool host_ok{};
    while(!headers.empty()){
        auto e=headers.find("\r\n");if(e==headers.npos||e==0)break;auto h=headers.substr(0,e);headers.remove_prefix(e+2);
        if(h.size()>=5&&_strnicmp(h.data(),"Host:",5)==0){h.remove_prefix(5);while(!h.empty()&&(h.front()==' '||h.front()=='\t'))h.remove_prefix(1);if(host_ok||h!=host)return {};host_ok=true;}
    }
    if(!host_ok)return {};
    std::map<Text,Text> fields;auto query=line.substr(6,line.size()-6-9);
    while(!query.empty()){
        auto sep=query.find('&');auto part=query.substr(0,sep);auto eq=part.find('=');
        if(eq==part.npos)return {};
        auto k=decoded(part.substr(0,eq)),v=decoded(part.substr(eq+1));
        if(!k||!v||!fields.emplace(*k,*v).second)return {};
        if(sep==query.npos)break;
        query.remove_prefix(sep+1);
    }
    if(fields[L"state"]!=state)return {};
    if(!fields[L"error"].empty())return Callback{{},true};
    if(fields[L"code"].empty())return {};
    return Callback{fields[L"code"],false};
}
struct Network {
    WSADATA wsa{};Network(){require(WSAStartup(MAKEWORD(2,2),&wsa)==0,"Windows sockets unavailable.");}
    ~Network(){WSACleanup();}
};
struct Socket {
    SOCKET value{INVALID_SOCKET};explicit Socket(SOCKET s):value(s){}
    ~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}
    Socket(const Socket&)=delete;
};
struct Listener {
    Network network;Socket socket{::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)};
    std::string host;Text redirect;
    Listener() {
        require(socket.value!=INVALID_SOCKET,"Cannot create OAuth callback socket.");BOOL exclusive=TRUE;
        require(setsockopt(socket.value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<char*>(&exclusive),sizeof(exclusive))==0,"Cannot reserve OAuth callback socket.");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        require(bind(socket.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(socket.value,4)==0,"Cannot listen for Google authorization on loopback.");
        int length=sizeof(address);require(getsockname(socket.value,reinterpret_cast<sockaddr*>(&address),&length)==0,"Cannot read loopback port.");
        host="127.0.0.1:"+std::to_string(ntohs(address.sin_port));redirect=L"http://"+wide(host)+L"/";
    }
    Text wait(View state) {
        auto until=std::chrono::steady_clock::now()+std::chrono::minutes(3);
        while(std::chrono::steady_clock::now()<until) {
            fd_set set;FD_ZERO(&set);FD_SET(socket.value,&set);timeval duration{1,0};
            int ready=select(0,&set,nullptr,nullptr,&duration);require(ready!=SOCKET_ERROR,"OAuth callback listener failed.");if(!ready)continue;
            Socket peer(accept(socket.value,nullptr,nullptr));if(peer.value==INVALID_SOCKET)continue;
            DWORD timeout=1000;setsockopt(peer.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
            setsockopt(peer.value,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
            std::string request_text;std::array<char,1024> buf{};
            auto peer_until=std::min(until,std::chrono::steady_clock::now()+std::chrono::seconds(3));
            while(request_text.size()<8192&&request_text.find("\r\n\r\n")==request_text.npos&&std::chrono::steady_clock::now()<peer_until) {
                const int n=recv(peer.value,buf.data(),static_cast<int>(buf.size()),0);if(n<=0)break;request_text.append(buf.data(),n);
            }
            auto reply=request_text.ends_with("\r\n\r\n")?callback(request_text,state,host):std::nullopt;
            std::string message=reply?"HTTP/1.1 200 OK\r\n":"HTTP/1.1 400 Bad Request\r\n";
            message+="Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
            message+=reply?"Return to Logi Options Open to finish.":"Invalid authorization callback.";
            send(peer.value,message.data(),static_cast<int>(message.size()),0);
            if(reply){require(!reply->denied,"Google authorization was declined.");return reply->code;}
        }
        throw std::runtime_error("Google authorization timed out.");
    }
};
Object token_exchange(const Client& client,Text parameters) {
    parameters+=L"&client_id="+encoded(client.id);
    if(!client.secret.empty())parameters+=L"&client_secret="+encoded(client.secret);
    const auto body=utf8(parameters);auto reply=request(L"POST",L"https://oauth2.googleapis.com/token",L"Content-Type: application/x-www-form-urlencoded\r\n",byte_view(body));
    if(reply.status!=200) {
        if(reply.status==400||reply.status==401) {
            auto error=field(parse(reply.bytes),L"error",false);
            if(error==L"invalid_grant")throw std::runtime_error("Google authorization expired or was revoked. Connect Google Drive again.");
            if(error==L"invalid_client")throw std::runtime_error("Google rejected the Desktop OAuth client ID or client secret.");
        }
        throw std::runtime_error("Google token exchange failed (HTTP "+std::to_string(reply.status)+").");
    }
    auto obj=parse(reply.bytes);auto granted=field(obj,L"scope",false);
    require(granted.empty()||granted==scope,"Google returned an unexpected authorization scope.");
    require(_wcsicmp(field(obj,L"token_type").c_str(),L"Bearer")==0,"Google returned an unsupported token type.");
    return obj;
}
Text bearer(const Object& token) {
    auto access=field(token,L"access_token");
    require(std::ranges::all_of(access,[](wchar_t c){return c>32&&c<127;}),"Invalid Google access token.");
    return L"Authorization: Bearer "+access+L"\r\n";
}
void save_grant(View key,View account,View refresh) {
    require(valid_id(account)&&!refresh.empty()&&std::ranges::all_of(refresh,[](wchar_t c){return c>32&&c<127;}),"Invalid Google authorization grant.");
    write_secret(key,Text(account)+L"\n"+Text(refresh));
}
struct Access {Text header,account;};
Access access_header(const Client& client) {
    auto key=secret_key(client);GrantLock lock(client);
    auto grant=read_secret(key);auto split=grant.find(L'\n');
    require(split!=grant.npos&&split+1<grant.size(),"Connect Google Drive before synchronizing.");
    auto refresh=grant.substr(split+1);
    auto token=token_exchange(client,L"grant_type=refresh_token&refresh_token="+encoded(refresh));auto updated=field(token,L"refresh_token",false);
    if(!updated.empty()&&updated!=refresh)save_grant(key,View(grant).substr(0,split),updated);
    return {bearer(token),grant.substr(0,split)};
}
void http_ok(const HttpReply& reply) {
    if(reply.status==401)throw std::runtime_error("Google Drive authorization no longer works. Connect again.");
    if(reply.status==403)throw std::runtime_error("Google Drive denied access. Check that the Drive API is enabled for this Desktop OAuth client.");
    if(reply.status==409||reply.status==412)throw std::runtime_error("Google Drive conflict. Read the latest cloud configuration before writing.");
    if(reply.status<200||reply.status>=300)throw std::runtime_error("Google Drive returned HTTP "+std::to_string(reply.status)+".");
}
struct Node {
    Text id,next,generation,digest,accepted;
};
struct Branch {Text id,created;Node head;};
struct State {
    Branch primary;Text others,tag;bool conflict{};
};
Object response(HttpReply reply){http_ok(reply);return parse(reply.bytes);}
Text identifier(const Object& obj,View name) {
    auto id=field(obj,name);require(valid_id(id),"Drive returned an invalid file identifier.");return id;
}
Text generated(View auth) {
    auto obj=response(request(L"GET",Text(api)+L"/generateIds?count=1&space=appDataFolder&type=files",auth));
    auto ids=array(obj,L"ids");HString value;check(ids->GetStringAt(0,&value.h));auto id=value.value();
    require(valid_id(id),"Drive returned an invalid generated ID.");return id;
}
std::vector<Object> find_files(View auth,View search,View order={},bool one=false) {
    Text base=Text(api)+L"?spaces=appDataFolder&fields="+encoded(L"nextPageToken,incompleteSearch,files(id,name,createdTime,appProperties)")+
        L"&q="+encoded(search)+L"&pageSize="+(one?L"1":L"100");
    if(!order.empty())base+=L"&orderBy="+encoded(order);
    Text page;std::vector<Object> out;std::vector<Text> pages;
    do {
        auto obj=response(request(L"GET",base+(page.empty()?L"":L"&pageToken="+encoded(page)),auth));
        HString key(L"incompleteSearch");boolean partial{};
        if(SUCCEEDED(obj->GetNamedBoolean(key.h,&partial)))require(!partial,"Drive returned an incomplete search. Retry synchronization.");
        auto rows=array(obj,L"files");
        for(UINT32 i=0;;++i){Object row;auto hr=rows->GetObjectAt(i,&row);if(hr==E_BOUNDS)break;check(hr);out.push_back(row);}
        if(one&&!out.empty())break;
        page=field(obj,L"nextPageToken",false);
        if(!page.empty()){require(std::ranges::find(pages,page)==pages.end(),"Drive repeated a page token.");pages.push_back(page);}
    }while(!page.empty());
    return out;
}
Node node(const Object& obj,View root) {
    Node n;auto props=property_object(obj);
    require(identifier(props,L"root")==root,"Drive snapshot belongs to a different configuration.");
    n.id=identifier(obj,L"id");n.next=identifier(props,L"next");n.digest=field(props,L"digest");
    n.accepted=field(props,L"accepted",false);n.generation=field(obj,L"name");
    require(n.generation.size()==file_name.size()+1+16 && n.generation.starts_with(Text(file_name)+L"-") &&
        std::ranges::all_of(View(n.generation).substr(file_name.size()+1),[](wchar_t c){return c>=L'0'&&c<=L'9';}),"Invalid Drive snapshot generation.");
    return n;
}
Text next_name(const Node& last) {
    unsigned long long value=last.id.empty()?0:std::stoull(last.generation.substr(file_name.size()+1));
    require(value<9999999999999999ULL,"Google Drive snapshot limit reached.");
    auto number=std::to_wstring(value+1);return Text(file_name)+L"-"+Text(16-number.size(),L'0')+number;
}
Text hash(std::span<const unsigned char> bytes) {
    BCRYPT_ALG_HANDLE alg{};
    require(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0,"Windows SHA-256 unavailable.");
    std::array<unsigned char,32> digest{};
    const auto status=BCryptHash(alg,nullptr,0,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),digest.data(),digest.size());
    BCryptCloseAlgorithmProvider(alg,0);require(status==0,"Windows SHA-256 failed.");return b64(digest);
}
State current(View auth) {
    auto found=find_files(auth,L"trashed = false and name = '"+Text(anchor_name)+L"'");
    std::vector<Branch> branches;
    for(auto& obj:found) {
        Branch branch{identifier(obj,L"id"),field(obj,L"createdTime"),{}};
        branch.head.next=identifier(property_object(obj),L"next");
        auto latest=find_files(auth,L"trashed = false and appProperties has { key='root' and value='"+branch.id+L"' }",L"name desc",true);
        if(!latest.empty())branch.head=node(latest.front(),branch.id);
        // A media create by ID is atomic. Verify the next ID directly as the
        // list index may still point to the preceding generation.
        for(unsigned hop=0;;++hop) {
            require(hop<32,"Drive is changing during synchronization. Retry.");
            auto reply=request(L"GET",Text(api)+L"/"+branch.head.next+L"?fields=id,name,appProperties",auth);
            if(reply.status==404)break;
            auto child=node(response(std::move(reply)),branch.id);
            require(child.id==branch.head.next&&child.generation==next_name(branch.head),"Invalid Drive snapshot chain.");
            branch.head=std::move(child);
        }
        branches.push_back(std::move(branch));
    }
    if(branches.empty())return {};
    // Empty anchors carry no user data. Prefer occupied branches, then use the
    // same stable election on all devices. Two occupied roots are retained.
    std::ranges::sort(branches,[](const Branch& a,const Branch& b){
        if(a.head.id.empty()!=b.head.id.empty())return !a.head.id.empty();
        return std::tie(a.created,a.id)<std::tie(b.created,b.id);
    });
    State state;state.primary=branches.front();std::string siblings;
    for(std::size_t i=1;i<branches.size();++i)if(!branches[i].head.id.empty()) {
        siblings+=utf8(branches[i].id)+":"+utf8(branches[i].head.id)+"\n";
        if(branches[i].head.digest!=state.primary.head.digest)state.conflict=true;
    }
    if(!siblings.empty())state.others=hash(byte_view(siblings));
    state.conflict=state.conflict&&state.primary.head.accepted!=state.others;
    if(!state.primary.head.id.empty())state.tag=state.primary.id+L":"+state.primary.head.id+L":"+state.others;
    return state;
}
void create_anchor(View auth) {
    const auto next=generated(auth);
    const auto json=utf8(L"{\"name\":\""+Text(anchor_name)+L"\",\"parents\":[\"appDataFolder\"],\"appProperties\":{\"next\":\""+next+L"\"}}");
    auto reply=request(L"POST",Text(api)+L"?fields=id",Text(auth)+L"Content-Type: application/json; charset=UTF-8\r\n",byte_view(json));
    auto obj=response(std::move(reply));identifier(obj,L"id");
}
void create_snapshot(View auth,const State& state,std::span<const unsigned char> bytes) {
    const auto next=generated(auth);const auto& branch=state.primary;
    const auto metadata=utf8(L"{\"id\":\""+branch.head.next+L"\",\"name\":\""+next_name(branch.head)+L"\",\"parents\":[\"appDataFolder\"],"
        L"\"mimeType\":\"application/octet-stream\",\"appProperties\":{\"root\":\""+branch.id+L"\",\"next\":\""+next+
        L"\",\"digest\":\""+hash(bytes)+L"\",\"accepted\":\""+state.others+L"\"}}");
    auto boundary="LogiOpen-"+utf8(random_code());
    std::string prefix="--"+boundary+"\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n"+metadata+
        "\r\n--"+boundary+"\r\nContent-Type: application/octet-stream\r\n\r\n";
    std::string tail="\r\n--"+boundary+"--\r\n";Bytes body;
    body.reserve(prefix.size()+bytes.size()+tail.size());body.insert(body.end(),prefix.begin(),prefix.end());
    body.insert(body.end(),bytes.begin(),bytes.end());body.insert(body.end(),tail.begin(),tail.end());
    auto reply=request(L"POST",upload,Text(auth)+L"Content-Type: multipart/related; boundary="+wide(boundary)+L"\r\n",body);
    auto obj=response(std::move(reply));require(identifier(obj,L"id")==branch.head.next,"Drive returned a different snapshot ID.");
}

void authorize_using(const Client& client,auto&& open_browser) {
    Apartment apt;const auto key=secret_key(client);Listener listener;auto verifier=random_code(),state=random_code();
    const auto url=L"https://accounts.google.com/o/oauth2/v2/auth?response_type=code&access_type=offline&prompt=consent&client_id="+encoded(client.id)+
        L"&scope="+encoded(scope)+L"&redirect_uri="+encoded(listener.redirect)+L"&state="+encoded(state)+L"&code_challenge_method=S256&code_challenge="+challenge(verifier);
    open_browser(url);
    auto code=listener.wait(state);
    auto obj=token_exchange(client,L"grant_type=authorization_code&code="+encoded(code)+L"&code_verifier="+encoded(verifier)+L"&redirect_uri="+encoded(listener.redirect));
    auto about=response(request(L"GET",L"https://www.googleapis.com/drive/v3/about?fields=user(permissionId)",bearer(obj)));
    HString user_key(L"user");Object user;check(about->GetNamedObject(user_key.h,&user));
    GrantLock lock(client);
    save_grant(key,identifier(user,L"permissionId"),field(obj,L"refresh_token"));
}
} // namespace

void authorize(const Client& client) {
    authorize_using(client,[](const Text& url){
        require(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL))>32,"Cannot open the default browser for Google authorization.");
    });
}
void disconnect(const Client& client){auto key=secret_key(client);GrantLock lock(client);erase_secret(key);}
std::wstring account_id(const Client& client) {
    const auto grant=read_secret(secret_key(client));
    const auto end=grant.find(L'\n');
    return end==grant.npos?Text{}:grant.substr(0,end);
}
Remote read(const Client& client) {
    Apartment apt;auto access=access_header(client);auto state=current(access.header);
    const auto tag=access.account+L"/"+state.tag;
    if(state.primary.head.id.empty())return {{},tag,false};
    auto reply=request(L"GET",Text(api)+L"/"+state.primary.head.id+L"?alt=media",access.header);http_ok(reply);
    require(reply.bytes.size()<=max_bytes&&hash(reply.bytes)==state.primary.head.digest,"Drive snapshot failed its SHA-256 content check.");
    return {std::move(reply.bytes),tag,true,state.conflict};
}
void write(const Client& client,const Remote& expected,std::span<const unsigned char> bytes) {
    require(!bytes.empty()&&bytes.size()<=max_bytes,"Invalid Drive configuration size.");
    Apartment apt;auto access=access_header(client);const auto& auth=access.header;
    require(expected.etag.starts_with(access.account+L"/"),"Google Drive account changed. Read the configuration again.");
    auto state=current(auth);
    if(state.primary.id.empty()&&!expected.exists){create_anchor(auth);state=current(auth);}
    require(!state.primary.id.empty(),"Drive has not indexed the new configuration yet. Retry.");
    require(expected.exists==!state.primary.head.id.empty()&&expected.etag==access.account+L"/"+state.tag,"Google Drive conflict. The cloud configuration changed.");
    const auto committed=state.primary.head.next;create_snapshot(auth,state,bytes);
    const auto observed=current(auth);
    require(observed.primary.head.id==committed&&!observed.conflict,"Concurrent Google Drive changes were preserved. Resolve the conflict before synchronizing again.");
}
} // namespace config_sync::drive
