#pragma once
#include "internal.hpp"
#include "weapp/device_apis.hpp"
#include <algorithm>
#include <atomic>
#include <deque>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <system_error>
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace weapp::desktop::detail {
inline Json device_failure(const std::string& api,const std::string& message,const std::string& code="invalid_request",int64_t native=0) {
  Json detail={{"errMsg",api+":fail "+message}};
  if(native) detail["errCode"]=native;
  return {{"code",code},{"message",message},{"retryable",false},{"details",detail}};
}
inline Json device_ok(const std::string& api,Json value=Json::object()) {
  value["errMsg"]=api+":ok";return value;
}
inline std::string exact_epoch(const Json& value) {
  if(value.is_string()) {
    auto text=value.get<std::string>();
    if(text.empty() || text.size()>20 || (text.size()>1 && text[0]=='0') || !std::all_of(text.begin(),text.end(),[](unsigned char c){return c>='0'&&c<='9';})) throw std::invalid_argument("invalid account epoch");
    size_t used=0;(void)std::stoull(text,&used);if(used!=text.size()) throw std::invalid_argument("invalid account epoch");return text;
  }
  if(value.is_number_unsigned() && value.get<uint64_t>()<=9007199254740991ULL) return std::to_string(value.get<uint64_t>());
  if(value.is_number_integer() && value.get<int64_t>()>=0 && value.get<int64_t>()<=9007199254740991LL) return std::to_string(value.get<int64_t>());
  throw std::invalid_argument("account epoch must be an exact decimal string");
}
inline Json inline_binary(const std::vector<uint8_t>& bytes) {return {{"__weappBinary",{{"type","ArrayBuffer"},{"bytes",bytes}}}};}
inline std::vector<uint8_t> binary_bytes(const Json& value) {
  if(!value.is_object()||value.size()!=1||!value.contains("__weappBinary")) throw std::invalid_argument("expected ArrayBuffer data");
  const auto& marker=value.at("__weappBinary");
  if(marker.value("type","")!="ArrayBuffer"||!marker.contains("bytes")||!marker["bytes"].is_array()||marker["bytes"].size()>1048576) throw std::invalid_argument("invalid binary chunk");
  std::vector<uint8_t> out;out.reserve(marker["bytes"].size());
  for(const auto& item:marker["bytes"]) {if(!item.is_number_integer()||item.get<int>()<0||item.get<int>()>255) throw std::invalid_argument("invalid binary byte");out.push_back(item.get<uint8_t>());}
  return out;
}
inline std::string scope_material(const DeviceApiOptions& options) {
  const auto app=encode(options.application),account=encode(options.account);
  Json scope=Json::array();for(auto name:{"hostAppId","tenantId","publisherId","appId","environment"}) {
    auto value=app.value(name,"");if(value.empty()||value.size()>512) throw std::invalid_argument("native devices require trusted installed application");scope.push_back(value);
  }
  scope.push_back(account.value("principalId",Json(nullptr)));exact_epoch(account.at("epoch"));return scope.dump();
}
struct DeviceQueued {
  enum Kind {Complete,Failure,NativeEvent,Snapshots,OwnerAction} kind=Complete;
  std::string id,event,object;
  Json value;
  size_t bytes=0;
  std::function<void()> action;
};
struct DeviceQueue {
  std::atomic<bool> closed{false};std::mutex mutex,side_effects;std::deque<DeviceQueued> items;size_t bytes=0;
  bool push(DeviceQueued item) {
    if(closed) return false;
    try { item.bytes=item.value.dump().size(); }
    catch(const std::exception&) { item.kind=DeviceQueued::Failure;item.value=device_failure("native","native result is not valid UTF-8 JSON");item.bytes=item.value.dump().size(); }
    std::lock_guard<std::mutex> guard(mutex);if(closed) return false;
    constexpr size_t limit=8*1024*1024,reserve=256*512;
    const auto available=limit-std::min(bytes,limit);
    if((item.kind==DeviceQueued::NativeEvent||item.kind==DeviceQueued::Snapshots||item.kind==DeviceQueued::OwnerAction) && (items.size()>=512||item.bytes>available||available-item.bytes<reserve)) return false;
    if((item.kind==DeviceQueued::Complete||item.kind==DeviceQueued::Failure) && (item.bytes>available||available-item.bytes<reserve)) {
      item.kind=DeviceQueued::Failure;item.value=device_failure("native","native callback backpressure limit exceeded");item.bytes=item.value.dump().size();
    }
    if(items.size()>=1024||item.bytes>available) return false;
    bytes+=item.bytes;items.push_back(std::move(item));return true;
  }
};
// Retirement waits for bounded native disk publications already in progress;
// a late OS callback cannot start a new write after cleanup acknowledgement.
class DeviceSideEffect {
  std::shared_ptr<DeviceQueue> owner_;std::unique_lock<std::mutex> lock_;
 public:
  explicit DeviceSideEffect(const std::weak_ptr<DeviceQueue>& weak):owner_(weak.lock()),lock_(owner_?std::unique_lock<std::mutex>(owner_->side_effects):std::unique_lock<std::mutex>()){}
  explicit operator bool() const{return owner_&&!owner_->closed;}
};
inline bool queue_complete(const std::weak_ptr<DeviceQueue>& weak,const std::string& id,Json value) {
  if(auto queue=weak.lock()) return queue->push({DeviceQueued::Complete,id,"","",std::move(value)});return false;
}
inline bool queue_failure(const std::weak_ptr<DeviceQueue>& weak,const std::string& id,Json error) {
  if(auto queue=weak.lock()) return queue->push({DeviceQueued::Failure,id,"","",std::move(error)});return false;
}
inline bool queue_event(const std::weak_ptr<DeviceQueue>& weak,const std::string& name,const std::string& object,Json value) {
  if(auto queue=weak.lock()) return queue->push({DeviceQueued::NativeEvent,"",name,object,std::move(value)});return false;
}
inline bool queue_action(const std::weak_ptr<DeviceQueue>& weak,std::function<void()> action) {
  if(auto queue=weak.lock()) {DeviceQueued item;item.kind=DeviceQueued::OwnerAction;item.action=std::move(action);return queue->push(std::move(item));}return false;
}
// Match ManagedSyncFileSystem's scope lock across asynchronous C++ workers and
// synchronous Rust VM callbacks. Each instance owns its own open-file handle.
class ManagedFileLock {
#if defined(_WIN32)
  HANDLE file_=INVALID_HANDLE_VALUE;OVERLAPPED range_{};
#else
  int file_=-1;
#endif
 public:
  explicit ManagedFileLock(const std::filesystem::path& root) {
    auto path=root/".weapp-operations.lock";
#if defined(_WIN32)
    file_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_HIDDEN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file_==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category());
    BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file_,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!LockFileEx(file_,LOCKFILE_EXCLUSIVE_LOCK,0,MAXDWORD,MAXDWORD,&range_)){auto error=GetLastError();CloseHandle(file_);file_=INVALID_HANDLE_VALUE;throw std::system_error(error,std::system_category());}
#else
    file_=::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(file_<0)throw std::system_error(errno,std::generic_category());
    while(::flock(file_,LOCK_EX)!=0){if(errno==EINTR)continue;auto error=errno;::close(file_);file_=-1;throw std::system_error(error,std::generic_category());}
#endif
  }
  ManagedFileLock(const ManagedFileLock&)=delete;ManagedFileLock& operator=(const ManagedFileLock&)=delete;
  ~ManagedFileLock(){
#if defined(_WIN32)
    if(file_!=INVALID_HANDLE_VALUE){UnlockFileEx(file_,0,MAXDWORD,MAXDWORD,&range_);CloseHandle(file_);}
#else
    if(file_>=0){::flock(file_,LOCK_UN);::close(file_);}
#endif
  }
};
inline void durable_write(const std::filesystem::path& path,const std::vector<uint8_t>& bytes) {
  static std::atomic<uint64_t> sequence{1};
  auto temporary=path.parent_path()/(".weapp-write-"+std::to_string(sequence.fetch_add(1)));
  try {
#if defined(_WIN32)
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(),std::system_category());
    size_t offset=0;while(offset<bytes.size()) {DWORD wrote=0;if(!WriteFile(file,bytes.data()+offset,DWORD(std::min<size_t>(bytes.size()-offset,1048576)),&wrote,nullptr)||!wrote) {auto error=GetLastError();CloseHandle(file);throw std::system_error(error,std::system_category());}offset+=wrote;}
    if(!FlushFileBuffers(file)) {auto error=GetLastError();CloseHandle(file);throw std::system_error(error,std::system_category());}CloseHandle(file);
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::system_error(GetLastError(),std::system_category());
#else
    int file=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(file<0) throw std::system_error(errno,std::generic_category());
    size_t offset=0;while(offset<bytes.size()) {auto wrote=::write(file,bytes.data()+offset,bytes.size()-offset);if(wrote<0&&errno==EINTR) continue;if(wrote<=0){auto error=errno;::close(file);throw std::system_error(error,std::generic_category());}offset+=size_t(wrote);}
    if(::fsync(file)!=0){auto error=errno;::close(file);throw std::system_error(error,std::generic_category());}::close(file);
    if(::rename(temporary.c_str(),path.c_str())!=0) throw std::system_error(errno,std::generic_category());
    int directory=::open(path.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(directory<0) throw std::system_error(errno,std::generic_category());
    auto result=::fsync(directory),error=errno;::close(directory);if(result!=0) throw std::system_error(error,std::generic_category());
#endif
  }catch(...) {std::error_code ignored;std::filesystem::remove(temporary,ignored);throw;}
}
inline std::filesystem::path managed_path(const std::filesystem::path& root,const std::string& value,bool allow_root=false) {
  if(allow_root&&(value=="wxfile://user"||value=="wxfile://user/")) return root;
  if(value.rfind("wxfile://user/",0)!=0) throw std::invalid_argument("path must be managed wxfile://user/");
  auto suffix=value.substr(14);if(suffix.empty()||suffix.size()>4096||suffix.find('\\')!=std::string::npos||suffix.find('\0')!=std::string::npos) throw std::invalid_argument("invalid managed file path");
  std::filesystem::path result=root;std::stringstream stream(suffix);std::string part;size_t depth=0;
  while(std::getline(stream,part,'/')) {if(part.empty()||part=="."||part==".."||part.rfind(".weapp-",0)==0||part.find(':')!=std::string::npos||++depth>32) throw std::invalid_argument("invalid managed file segment");result/=std::filesystem::u8path(part);
    std::error_code error;auto status=std::filesystem::symlink_status(result,error);if(!error&&std::filesystem::is_symlink(status)) throw std::invalid_argument("managed files cannot traverse symbolic links");}
  return result;
}
inline std::vector<uint8_t> read_managed(const std::filesystem::path& path,size_t limit=1048576) {
  auto size=std::filesystem::file_size(path);if(size>limit) throw std::invalid_argument("file exceeds chunk limit; use a bounded stream");
  std::ifstream input(path,std::ios::binary);if(!input) throw std::runtime_error("cannot open managed file");
  std::vector<uint8_t> bytes(size);if(size&&!input.read(reinterpret_cast<char*>(bytes.data()),size)) throw std::runtime_error("cannot read managed file");return bytes;
}
inline void check_managed_quota(const std::filesystem::path& root,const std::filesystem::path& target,size_t size) {
  constexpr uint64_t quota=32*1024*1024;uint64_t total=0;size_t entries=0;
  for(auto iterator=std::filesystem::recursive_directory_iterator(root);iterator!=std::filesystem::recursive_directory_iterator();++iterator){const auto& entry=*iterator;if(++entries>4096||iterator.depth()>32)throw std::invalid_argument("managed file entry budget exceeded");if(entry.is_symlink()){iterator.disable_recursion_pending();continue;}if(entry.path().filename().u8string().rfind(".weapp-",0)==0){iterator.disable_recursion_pending();continue;}if(entry.is_regular_file()){auto bytes=entry.file_size();if(bytes>quota||total>quota-bytes)throw std::invalid_argument("managed file quota exceeded");total+=bytes;}}
  auto previous=std::filesystem::exists(target)&&std::filesystem::is_regular_file(target)?std::filesystem::file_size(target):0;
  if(size>quota||total-std::min<uint64_t>(total,previous)>quota-size)throw std::invalid_argument("managed file quota exceeded");
}
inline void managed_write(const std::filesystem::path& root,const std::filesystem::path& target,const std::vector<uint8_t>& bytes) {
  ManagedFileLock transaction(root);check_managed_quota(root,target,bytes.size());durable_write(target,bytes);
}
inline std::vector<uint8_t> managed_read(const std::filesystem::path& root,const std::filesystem::path& path,size_t limit=1048576) {
  ManagedFileLock transaction(root);return read_managed(path,limit);
}

class DeviceBase : public DeviceApis {
 protected:
  DeviceApiOptions options_;Command command_;std::thread::id owner_=std::this_thread::get_id();std::shared_ptr<DeviceQueue> queue_=std::make_shared<DeviceQueue>();
  std::map<std::string,Json> apis_,snapshots_;std::filesystem::path root_,consents_file_;std::vector<std::future<void>> workers_;
  struct Call {Json context;bool retained=false;std::function<void()> cancel;};std::map<std::string,Call> calls_;
  struct Subscription {std::string event,object,call;Json context;uint64_t sequence=0;};std::map<std::string,Subscription> subscriptions_;
  bool foreground_=true,retired_successfully_=false;
  std::map<std::string,std::string> objects_;std::map<std::string,bool> consents_;std::string bound_session_;
  DeviceBase(DeviceApiOptions options,Command command,std::filesystem::path = {}):options_(std::move(options)),command_(std::move(command)) {
    scope_material(options_);Value leased;
    auto status=command_("managedFileScope",{},&leased);if(!status)throw std::runtime_error("managed file lease: "+status.message);
    auto scope=encode(leased),application=encode(options_.application),account=encode(options_.account);
    for(auto name:{"hostAppId","tenantId","publisherId","appId","environment","version","packRoot"})if(scope.at("application").value(name,"")!=application.value(name,""))throw std::invalid_argument("managed file lease application does not match trusted installation");
    if(scope.at("account").value("principalId",Json(nullptr))!=account.value("principalId",Json(nullptr))||exact_epoch(scope.at("account").at("epoch"))!=exact_epoch(account.at("epoch")))throw std::invalid_argument("managed file lease belongs to a stale account");
    options_.managed_scope_root=scope.at("root").get<std::string>();options_.auxiliary_root=scope.at("auxiliaryRoot").get<std::string>();
    root_=std::filesystem::u8path(options_.managed_scope_root);auto auxiliary=std::filesystem::u8path(options_.auxiliary_root);
    if(!root_.is_absolute()||!auxiliary.is_absolute()||!std::filesystem::is_directory(root_)||!std::filesystem::is_directory(auxiliary))throw std::invalid_argument("core managed file scope directories are unavailable");
    consents_file_=auxiliary/"consents.json";
    std::ifstream consent(consents_file_);if(consent) {auto values=Json::parse(consent,nullptr,false);if(values.is_object()) for(auto i=values.begin();i!=values.end();++i) if(i.value().is_boolean()) consents_[i.key()]=i.value().get<bool>();}
    for(auto api:{"wx.getFileSystemManager","wx.getSetting","wx.authorize","wx.openSetting"}) add(api);
    for(auto member:{"access","appendFile","copyFile","mkdir","readFile","readdir","rename","rmdir","unlink","writeFile","saveFile","removeSavedFile","getSavedFileList"}) add(std::string("object:FileSystemManager.")+member);
  }
  void add(const std::string& api) {apis_[api]=true;}
  void snapshot(const std::string& api,Json value) {add(api);snapshots_[api]=std::move(value);}
  void add_event(const std::string& on,const std::string& off,const std::string& event) {apis_[on]=event;apis_[off]=event;}
  std::weak_ptr<DeviceQueue> weak_queue() const {return queue_;}
  void done(const std::string& id,const std::string& api,Json value=Json::object()) {queue_complete(queue_,id,device_ok(api,std::move(value)));}
  void fail(const std::string& id,const std::string& api,const std::string& message,const std::string& code="invalid_request",int64_t native=0) {queue_failure(queue_,id,device_failure(api,message,code,native));}
  void cancelled(const std::string& id) {
    auto found=calls_.find(id);if(found==calls_.end()) return;
    auto cancel=std::move(found->second.cancel);calls_.erase(found);
    for(auto i=subscriptions_.begin();i!=subscriptions_.end();) {if(i->second.call==id)i=subscriptions_.erase(i);else ++i;}
    if(cancel)cancel();
  }
  void retain_object(const std::string& id,const std::string& call,const std::string& type,std::function<void()> dispose={}) {
    if(id.empty()||objects_.size()>=256||objects_.count(id)) throw std::invalid_argument("native object identity is unavailable");
    objects_[id]=type;calls_.at(call).retained=true;calls_.at(call).cancel=[this,id,dispose=std::move(dispose)]{objects_.erase(id);if(dispose)dispose();};
  }
  void require_object(const Json& payload,const std::string& type) const {
    auto id=payload.value("objectId","");auto found=objects_.find(id);if(found==objects_.end()||found->second!=type) throw std::invalid_argument("native object is unknown or belongs to another type");
  }
  bool allowed_origin(const std::string& origin) const {return std::find(options_.allowed_network_origins.begin(),options_.allowed_network_origins.end(),origin)!=options_.allowed_network_origins.end();}
  void permit(const std::string& scope,const std::string& api,const std::string& call,std::function<void()> action) {
    auto consent=consents_.find(scope);
    if(consent!=consents_.end()) {if(consent->second)action();else fail(call,api,"application permission was denied; reset it in application settings","permission_denied");return;}
    auto weak=weak_queue();
    request_permission(scope,[this,weak,scope,api,call,action=std::move(action)](bool granted){
      queue_action(weak,[this,weak,scope,api,call,granted,action]{
      auto queue=weak.lock();if(!queue||queue->closed||!calls_.count(call)) return;
      consents_[scope]=granted;
      try {auto data=Json(consents_).dump();durable_write(consents_file_,std::vector<uint8_t>(data.begin(),data.end()));}catch(const std::exception& error){fail(call,api,error.what());return;}
      if(granted)action();else fail(call,api,"user denied application permission","permission_denied");
      });
    });
  }
  virtual void request_permission(const std::string&,std::function<void(bool)>)=0;
  virtual bool platform_call(const std::string&,const std::string&,const Json&,const Json&)=0;
  virtual void retire_platform() {}
  virtual void permissions_reset() {}
  virtual Status foreground_changed(bool) {return {};}
  virtual void subscription_changed(const std::string&,bool) {}
  void start_file(const std::string& call,const std::string& api,Json payload) {
    auto root=root_;auto weak=weak_queue();workers_.push_back(std::async(std::launch::async,[root,weak,call,api,payload=std::move(payload)]{
      try {
        auto queue=weak.lock();if(!queue||queue->closed)return;ManagedFileLock transaction(root);
        Json result=Json::object();auto leaf=api.substr(api.find_last_of('.')+1);
        auto path_for=[&](const std::string& key,bool allow_root=false){return managed_path(root,payload.value(key,""),allow_root);};
        if(leaf=="readFile") {auto bytes=read_managed(path_for("filePath"));auto encoding=payload.value("encoding","");if(encoding.empty())result["data"]=inline_binary(bytes);else if(encoding=="utf8"||encoding=="utf-8")result["data"]=std::string(bytes.begin(),bytes.end());else throw std::invalid_argument("file encoding must be utf8 or ArrayBuffer");}
        else if(leaf=="writeFile"||leaf=="appendFile") {auto path=path_for("filePath");auto data=payload.at("data");std::vector<uint8_t> bytes;
          if(data.is_string()){auto text=data.get<std::string>();bytes.assign(text.begin(),text.end());}else bytes=binary_bytes(data);
          if(bytes.size()>1048576)throw std::invalid_argument("write exceeds one MiB chunk limit");
          if(leaf=="appendFile"&&std::filesystem::exists(path)){auto previous=read_managed(path,32*1024*1024);if(previous.size()>32*1024*1024-bytes.size())throw std::invalid_argument("managed file quota exceeded");previous.insert(previous.end(),bytes.begin(),bytes.end());bytes=std::move(previous);}
          if(!std::filesystem::exists(path.parent_path()))throw std::invalid_argument("parent directory does not exist");check_managed_quota(root,path,bytes.size());durable_write(path,bytes);}
        else if(leaf=="access") {if(!std::filesystem::exists(path_for("path")))throw std::invalid_argument("managed path does not exist");}
        else if(leaf=="unlink"||leaf=="removeSavedFile") {if(!std::filesystem::remove(path_for("filePath")))throw std::invalid_argument("managed file does not exist");}
        else if(leaf=="copyFile") {auto source=path_for("srcPath"),destination=path_for("destPath");auto bytes=read_managed(source,32*1024*1024);check_managed_quota(root,destination,bytes.size());durable_write(destination,bytes);}
        else if(leaf=="rename") {auto source=path_for("oldPath"),destination=path_for("newPath");std::filesystem::rename(source,destination);}
        else if(leaf=="mkdir") {auto path=path_for("dirPath");if(payload.value("recursive",false))std::filesystem::create_directories(path);else if(!std::filesystem::create_directory(path))throw std::invalid_argument("directory already exists");}
        else if(leaf=="rmdir") {auto path=path_for("dirPath");if(payload.value("recursive",false))std::filesystem::remove_all(path);else if(!std::filesystem::remove(path))throw std::invalid_argument("directory does not exist");}
        else if(leaf=="readdir") {result["files"]=Json::array();for(const auto& entry:std::filesystem::directory_iterator(path_for("dirPath",true))) {if(!entry.is_symlink()&&entry.path().filename().u8string().rfind(".weapp-",0)!=0)result["files"].push_back(entry.path().filename().u8string());if(result["files"].size()>4096)throw std::invalid_argument("directory entry budget exceeded");}}
        else if(leaf=="getSavedFileList") {result["fileList"]=Json::array();for(const auto& entry:std::filesystem::recursive_directory_iterator(root)) if(entry.is_regular_file()&&!entry.is_symlink()&&entry.path().filename().u8string().rfind(".weapp-",0)!=0) {result["fileList"].push_back({{"filePath","wxfile://user/"+std::filesystem::relative(entry.path(),root).generic_u8string()},{"size",entry.file_size()}});if(result["fileList"].size()>4096)throw std::invalid_argument("saved file entry budget exceeded");}}
        else if(leaf=="saveFile") {auto source=path_for("tempFilePath");auto target=payload.value("filePath","");if(target.empty())target="wxfile://user/saved-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());auto destination=managed_path(root,target);std::filesystem::rename(source,destination);result["savedFilePath"]=target;}
        else throw std::invalid_argument("unknown managed file operation");
        queue_complete(weak,call,device_ok(api,std::move(result)));
      }catch(const std::system_error& error){queue_failure(weak,call,device_failure(api,error.what(),"invalid_request",error.code().value()));}
       catch(const std::exception& error){queue_failure(weak,call,device_failure(api,error.what()));}
    }));
  }
 public:
  ~DeviceBase() override {retire();}
  Value::Object configuration() const override {
    Json result={{"apis",Json::array()},{"syncValues",snapshots_}};for(const auto& api:apis_)result["apis"].push_back(api.first);
    return std::get<Value::Object>(decode(result).data);
  }
  bool dispatch(const Event& event) override {
    if(std::this_thread::get_id()!=owner_)throw std::logic_error("native device dispatcher is owner-thread only");
    auto value=encode(event.fields);
    if(event.type==EventType::HostCallCancelled){cancelled(value.value("callId",""));return false;}
    if(event.type!=EventType::HostCall)return false;
    auto api=value.value("method","");if(!apis_.count(api))return false;
    auto call=value.value("callId","");if(queue_->closed)return true;
    try {
      auto context=value.at("context"),application=context.at("application"),account=context.at("account");
      auto expected=encode(options_.application);for(auto key:{"hostAppId","tenantId","publisherId","appId","environment","version","packRoot"})if(application.value(key,"")!=expected.value(key,""))throw std::invalid_argument("native call application does not match installed scope");
      auto expected_account=encode(options_.account);if(account.value("principalId",Json(nullptr))!=expected_account.value("principalId",Json(nullptr))||exact_epoch(account.at("epoch"))!=exact_epoch(expected_account.at("epoch")))throw std::invalid_argument("native call account owner is stale");
      auto session=context.value("sessionId","");if(session.empty()||(!bound_session_.empty()&&bound_session_!=session)||context.value("callId","")!=call||context.value("api","")!=api)throw std::invalid_argument("native context correlation is invalid");bound_session_=session;
      if(calls_.size()>=256||calls_.count(call))throw std::invalid_argument("native call concurrency limit exceeded");
      calls_[call]={context,value.value("retainUntilCancelled",false),{}};if(!foreground_&&(api=="wx.getClipboardData"||api=="wx.setClipboardData"))throw std::invalid_argument("clipboard requires a foreground application");auto payload=value.value("payload",Json::object());
      if(apis_.at(api).is_string()) {auto id=payload.value("subscriptionId","");if(id.empty())throw std::invalid_argument("subscription identity is required");
        if(payload.value("action","")=="unsubscribe")subscriptions_.erase(id);else {if(subscriptions_.size()>=256)throw std::invalid_argument("native subscription budget exceeded");subscriptions_[id]={apis_.at(api).get<std::string>(),payload.value("objectId",""),call,context};}subscription_changed(apis_.at(api).get<std::string>(),payload.value("action","")!="unsubscribe");done(call,api);return true;}
      if(api=="wx.getFileSystemManager"){auto id=payload.value("objectId","");retain_object(id,call,"FileSystemManager");done(call,api,{{"objectId",id}});return true;}
      if(api.rfind("object:FileSystemManager.",0)==0){require_object(payload,"FileSystemManager");start_file(call,api,payload);return true;}
      if(api=="wx.saveFile"||api=="wx.removeSavedFile"||api=="wx.getSavedFileList"){start_file(call,api,payload);return true;}
      if(api=="wx.getSetting"){done(call,api,{{"authSetting",consents_}});return true;}
      if(api=="wx.authorize"){auto scope=payload.value("scope","");if(scope.rfind("scope.",0)!=0)throw std::invalid_argument("authorization scope is required");permit(scope,api,call,[this,call,api]{done(call,api);});return true;}
      if(api=="wx.openSetting"){auto weak=weak_queue();request_permission("open-settings",[this,weak,call,api](bool shown){queue_action(weak,[this,weak,call,api,shown]{auto queue=weak.lock();if(queue&&!queue->closed&&calls_.count(call)){if(shown)done(call,api,{{"authSetting",consents_}});else fail(call,api,"settings require an attached native parent");}});});return true;}
      if(!platform_call(call,api,payload,context))fail(call,api,"native handler is unavailable","unsupported");
    }catch(const std::exception& error){if(!calls_.count(call))calls_[call]={Json::object(),false,{}};fail(call,api,error.what());}
    return true;
  }
  Status pump() override {
    if(std::this_thread::get_id()!=owner_)return Status::error("ERR_THREAD","native devices require their owner thread");
    if(queue_->closed)return {};
    for(auto i=workers_.begin();i!=workers_.end();) {if(i->wait_for(std::chrono::seconds(0))==std::future_status::ready){i->get();i=workers_.erase(i);}else ++i;}
    for(size_t count=0;count<512;++count){DeviceQueued item;
      {std::lock_guard<std::mutex> guard(queue_->mutex);if(queue_->items.empty())break;item=std::move(queue_->items.front());queue_->items.pop_front();queue_->bytes-=item.bytes;}
      if(item.kind==DeviceQueued::OwnerAction) {if(item.action)item.action();}
      else if(item.kind==DeviceQueued::NativeEvent) {
        for(auto& entry:subscriptions_) {auto& subscription=entry.second;if(subscription.event!=item.event||subscription.object!=item.object)continue;
          Json data={{"sessionId",subscription.context.at("sessionId")},{"accountEpoch",exact_epoch(subscription.context.at("account").at("epoch"))},{"subscriptionId",entry.first},{"sequence",std::to_string(++subscription.sequence)},{"value",item.value},{"event",item.event}};
          if(!item.object.empty())data["objectId"]=item.object;
          auto status=command_("hostApiEvent",std::get<Value::Object>(decode(data).data),nullptr);if(!status)return status;}
      }else if(item.kind==DeviceQueued::Snapshots){auto status=command_("updateHostServiceSnapshots",std::get<Value::Object>(decode(item.value).data),nullptr);if(!status)return status;}
      else {auto found=calls_.find(item.id);if(found==calls_.end())continue;bool retained=found->second.retained;if(item.kind==DeviceQueued::Failure||!retained)calls_.erase(found);
        Json data={{"callId",item.id},{item.kind==DeviceQueued::Failure?"error":"result",item.value}};
        auto status=command_("completeHostCall",std::get<Value::Object>(decode(data).data),nullptr);if(!status)return status;}
    }
    return {};
  }
  Status retire() override {
    if(retired_successfully_)return {};
    {std::lock_guard<std::mutex> barrier(queue_->side_effects);queue_->closed=true;}
    try {for(auto& call:calls_)if(call.second.cancel)call.second.cancel();calls_.clear();subscriptions_.clear();retire_platform();objects_.clear();
      for(auto& worker:workers_)worker.wait();workers_.clear();std::lock_guard<std::mutex> guard(queue_->mutex);queue_->items.clear();queue_->bytes=0;retired_successfully_=true;return {};
    }catch(const std::exception& error){return Status::error("ERR_RESOURCE_CLEANUP",error.what());}
  }
  Status set_native_parent(void* parent) override {options_.native_parent=parent;return {};}
  Status set_foreground(bool value) override {
    if(std::this_thread::get_id()!=owner_)return Status::error("ERR_THREAD","native device lifecycle requires its owner thread");
    if(queue_->closed)return Status::error("ERR_CLOSED","native devices are retired");
    if(foreground_==value)return {};auto status=foreground_changed(value);if(status)foreground_=value;return status;
  }
  Status update_components(const Value::Array&) override {return {};}
};
}  // namespace weapp::desktop::detail
