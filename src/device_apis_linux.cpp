#include <gtk/gtk.h>
#include <gio/gio.h>
#include "device_socket_common.hpp"
#if defined(WEAPP_DEVICE_CURL_WEBSOCKET)
#include "device_socket_curl.hpp"
#endif
#include <cmath>
#if defined(WEAPP_DEVICE_CURL)
#include <curl/curl.h>
#endif

namespace weapp::desktop::detail {
namespace {
struct DialogCompletion {std::function<void(int)> complete;};
void dialog_response(GtkDialog* dialog,gint response,gpointer pointer) {
  auto data=static_cast<DialogCompletion*>(pointer);data->complete(response);gtk_widget_destroy(GTK_WIDGET(dialog));
}
std::string uri_origin(const std::string& text) {
  GError* error=nullptr;GUri* uri=g_uri_parse(text.c_str(),G_URI_FLAGS_NONE,&error);
  if(!uri){std::string message=error?error->message:"invalid URL";if(error)g_error_free(error);throw std::invalid_argument(message);}
  auto release=std::unique_ptr<GUri,decltype(&g_uri_unref)>(uri,g_uri_unref);const char* scheme=g_uri_get_scheme(uri),*host=g_uri_get_host(uri);
  if(!scheme||g_ascii_strcasecmp(scheme,"https")!=0||!host||!*host||g_uri_get_userinfo(uri))throw std::invalid_argument("native requests require a credential-free HTTPS URL");
  std::string name=host;std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return std::tolower(c);});if(name.find(':')!=std::string::npos)name="["+name+"]";int port=g_uri_get_port(uri);return "https://"+name+(port>0&&port!=443?":"+std::to_string(port):"");
}
using Bus=std::shared_ptr<GDBusConnection>;
using Variant=std::unique_ptr<GVariant,decltype(&g_variant_unref)>;
Bus system_bus(GBusType type){GError* error=nullptr;auto connection=g_bus_get_sync(type,nullptr,&error);if(!connection){std::string message=error?error->message:"system message bus unavailable";if(error)g_error_free(error);throw std::runtime_error(message);}return Bus(connection,[](GDBusConnection* value){g_object_unref(value);});}
Variant bus_call(const Bus& bus,const char* service,const std::string& path,const char* interface,const char* method,GVariant* arguments,const GVariantType* expected=nullptr){GError* error=nullptr;auto result=g_dbus_connection_call_sync(bus.get(),service,path.c_str(),interface,method,arguments,expected,G_DBUS_CALL_FLAGS_NONE,5000,nullptr,&error);if(!result){std::string message=error?error->message:"native service operation failed";if(error)g_error_free(error);throw std::runtime_error(message);}return Variant(result,g_variant_unref);}
Variant nm_property(const Bus& bus,const std::string& path,const char* interface,const char* property){auto result=bus_call(bus,"org.freedesktop.NetworkManager",path,"org.freedesktop.DBus.Properties","Get",g_variant_new("(ss)",interface,property),G_VARIANT_TYPE("(v)"));GVariant* value=nullptr;g_variant_get(result.get(),"(v)",&value);return Variant(value,g_variant_unref);}
std::vector<std::string> wifi_devices(const Bus& bus){auto result=bus_call(bus,"org.freedesktop.NetworkManager","/org/freedesktop/NetworkManager","org.freedesktop.NetworkManager","GetDevices",nullptr,G_VARIANT_TYPE("(ao)"));GVariant* array=nullptr;g_variant_get(result.get(),"(@ao)",&array);Variant owned(array,g_variant_unref);GVariantIter iterator;g_variant_iter_init(&iterator,array);const char* path=nullptr;std::vector<std::string> devices;while(g_variant_iter_next(&iterator,"&o",&path)){if(devices.size()>=32)throw std::runtime_error("network adapter count exceeds native budget");auto type=nm_property(bus,path,"org.freedesktop.NetworkManager.Device","DeviceType");if(g_variant_get_uint32(type.get())==2)devices.push_back(path);}if(devices.empty())throw std::runtime_error("system has no NetworkManager Wi-Fi adapter");auto enabled=nm_property(bus,"/org/freedesktop/NetworkManager","org.freedesktop.NetworkManager","WirelessEnabled");if(!g_variant_get_boolean(enabled.get()))throw std::runtime_error("OS Wi-Fi is disabled");return devices;}
Json access_point(const Bus& bus,const std::string& path){auto result=bus_call(bus,"org.freedesktop.NetworkManager",path,"org.freedesktop.DBus.Properties","GetAll",g_variant_new("(s)","org.freedesktop.NetworkManager.AccessPoint"),G_VARIANT_TYPE("(a{sv})"));GVariant* raw=nullptr;g_variant_get(result.get(),"(@a{sv})",&raw);Variant properties(raw,g_variant_unref);GVariant* ssid=g_variant_lookup_value(raw,"Ssid",G_VARIANT_TYPE_BYTESTRING);if(!ssid)throw std::runtime_error("native Wi-Fi SSID is unavailable");Variant owned(ssid,g_variant_unref);gsize count=0;auto bytes=static_cast<const char*>(g_variant_get_fixed_array(ssid,&count,1));if(count>32||!g_utf8_validate(bytes,count,nullptr))throw std::runtime_error("native Wi-Fi SSID is not UTF-8");const char* bssid="";guint8 strength=0;guint32 flags=0;g_variant_lookup(raw,"HwAddress","&s",&bssid);g_variant_lookup(raw,"Strength","y",&strength);g_variant_lookup(raw,"Flags","u",&flags);return {{"SSID",std::string(bytes,count)},{"BSSID",bssid},{"signalStrength",strength},{"secure",bool(flags&1)}};}
Json native_wifi(const std::string& api){auto bus=system_bus(G_BUS_TYPE_SYSTEM);auto devices=wifi_devices(bus);if(api=="wx.startWifi")return Json::object();Json list=Json::array();for(const auto& device:devices){if(api=="wx.getConnectedWifi"){auto active=nm_property(bus,device,"org.freedesktop.NetworkManager.Device.Wireless","ActiveAccessPoint");auto path=std::string(g_variant_get_string(active.get(),nullptr));if(path!="/")return {{"wifi",access_point(bus,path)}};continue;}GVariantBuilder options;g_variant_builder_init(&options,G_VARIANT_TYPE("a{sv}"));bus_call(bus,"org.freedesktop.NetworkManager",device,"org.freedesktop.NetworkManager.Device.Wireless","RequestScan",g_variant_new("(a{sv})",&options));auto points=bus_call(bus,"org.freedesktop.NetworkManager",device,"org.freedesktop.NetworkManager.Device.Wireless","GetAllAccessPoints",nullptr,G_VARIANT_TYPE("(ao)"));GVariant* raw=nullptr;g_variant_get(points.get(),"(@ao)",&raw);Variant array(raw,g_variant_unref);GVariantIter iterator;g_variant_iter_init(&iterator,raw);const char* path=nullptr;while(g_variant_iter_next(&iterator,"&o",&path)&&list.size()<256){try{list.push_back(access_point(bus,path));}catch(const std::exception&){}}}if(api=="wx.getConnectedWifi")throw std::runtime_error("no connected Wi-Fi network");return {{"wifiList",list}};}
Json linux_battery() {
  for(const auto& entry:std::filesystem::directory_iterator("/sys/class/power_supply")) {
    std::ifstream type(entry.path()/"type");std::string name;std::getline(type,name);if(name!="Battery")continue;
    std::ifstream capacity(entry.path()/"capacity"),status(entry.path()/"status");int level=-1;capacity>>level;std::getline(status,name);
    if(level>=0&&level<=100)return {{"level",level},{"isCharging",name=="Charging"||name=="Full"}};
  }
  throw std::runtime_error("system battery information is unavailable");
}
}

class LinuxDeviceApis final:public DeviceSockets {
  bool wifi_active_=false;uint64_t wifi_generation_=0;Bus screen_bus_;guint32 screen_cookie_=0;
  GNetworkMonitor* network_=nullptr;gulong network_signal_=0;std::set<GtkWidget*> dialogs_;
  std::map<std::string,std::shared_ptr<std::atomic<bool>>> transfers_;
  GtkWindow* parent() const {if(!options_.native_parent)return nullptr;auto widget=GTK_WIDGET(options_.native_parent);auto top=gtk_widget_get_toplevel(widget);return GTK_IS_WINDOW(top)?GTK_WINDOW(top):nullptr;}
  void dialog(const std::string& title,const std::string& text,const std::vector<std::string>& buttons,std::function<void(int)> completion) {
    auto window=parent();if(!window)throw std::invalid_argument("native dialog requires an attached host GTK window");
    GtkWidget* widget=gtk_message_dialog_new(window,GTK_DIALOG_DESTROY_WITH_PARENT,GTK_MESSAGE_QUESTION,GTK_BUTTONS_NONE,"%s",title.c_str());gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(widget),"%s",text.c_str());
    for(size_t index=0;index<buttons.size();++index)gtk_dialog_add_button(GTK_DIALOG(widget),buttons[index].c_str(),int(index));
    auto weak=weak_queue();auto data=new DialogCompletion{std::move(completion)};
    g_signal_connect_data(widget,"response",G_CALLBACK(dialog_response),data,[](gpointer pointer,GClosure*){delete static_cast<DialogCompletion*>(pointer);},GConnectFlags(0));
    dialogs_.insert(widget);g_signal_connect_data(widget,"destroy",G_CALLBACK(+[](GtkWidget* widget,gpointer pointer){auto callback=static_cast<std::function<void()>*>(pointer);(*callback)();}),new std::function<void()>([this,weak,widget]{auto queue=weak.lock();if(queue&&!queue->closed)dialogs_.erase(widget);}),[](gpointer pointer,GClosure*){delete static_cast<std::function<void()>*>(pointer);},GConnectFlags(0));gtk_widget_show_all(widget);
  }
  void request_permission(const std::string& scope,std::function<void(bool)> completion) override {
    try{if(scope=="open-settings") {auto weak=weak_queue();dialog("Application permissions",Json(consents_).dump(2),{"Done","Reset permissions"},[this,weak,completion](int response){queue_action(weak,[this,weak,completion,response]{auto queue=weak.lock();if(!queue||queue->closed)return;if(response==1){reset_permissions();}completion(response>=0);});});}
      else dialog("Allow application access?",encode(options_.application).value("appId","")+" requests "+scope,{"Allow","Deny"},[completion](int response){completion(response==0);});}
    catch(...){completion(false);}
  }
#if defined(WEAPP_DEVICE_CURL)
  struct CurlTransfer {
    std::weak_ptr<DeviceQueue> queue;std::string call,api,object;Json headers=Json::object();std::vector<uint8_t> bytes;std::shared_ptr<std::atomic<bool>> cancelled;size_t limit=1048576;bool overflow=false;
  };
  static size_t curl_write(char* data,size_t size,size_t count,void* pointer) {
    auto& transfer=*static_cast<CurlTransfer*>(pointer);size_t bytes=size*count;if(transfer.cancelled->load()||bytes>transfer.limit-transfer.bytes.size()){transfer.overflow=true;return 0;}auto first=reinterpret_cast<uint8_t*>(data);transfer.bytes.insert(transfer.bytes.end(),first,first+bytes);
    for(size_t offset=0;offset<bytes;offset+=1048576)queue_event(transfer.queue,"ChunkReceived",transfer.object,{{"data",inline_binary(std::vector<uint8_t>(first+offset,first+offset+std::min<size_t>(1048576,bytes-offset)))}});return bytes;
  }
  static size_t curl_header(char* data,size_t size,size_t count,void* pointer) {
    auto& transfer=*static_cast<CurlTransfer*>(pointer);size_t bytes=size*count;std::string line(data,bytes);auto colon=line.find(':');
    if(colon!=std::string::npos){auto value=line.substr(colon+1);while(!value.empty()&&(value.back()=='\n'||value.back()=='\r'||value.back()==' '))value.pop_back();while(!value.empty()&&value.front()==' ')value.erase(value.begin());transfer.headers[line.substr(0,colon)]=value;}
    else if(line=="\r\n")queue_event(transfer.queue,"HeadersReceived",transfer.object,{{"header",transfer.headers}});return bytes;
  }
  static int curl_progress(void* pointer,curl_off_t total,curl_off_t current,curl_off_t upload_total,curl_off_t upload_current) {
    auto& transfer=*static_cast<CurlTransfer*>(pointer);if(transfer.cancelled->load())return 1;
    queue_event(transfer.queue,"ProgressUpdate",transfer.object,{{"progress",total>0?double(current)*100/total:0.0},{"totalBytesWritten",current},{"totalBytesExpectedToWrite",total},{"totalBytesSent",upload_current},{"totalBytesExpectedToSend",upload_total}});return 0;
  }
  void request(const std::string& call,const std::string& api,Json payload) {
    auto url=payload.at("url").get<std::string>();if(!allowed_origin(uri_origin(url)))throw std::invalid_argument("URL origin denied by trusted host policy");auto id=payload.at("objectId").get<std::string>();auto cancelled=std::make_shared<std::atomic<bool>>(false);transfers_[id]=cancelled;
    retain_object(id,call,api=="wx.request"?"RequestTask":api=="wx.downloadFile"?"DownloadTask":"UploadTask",[this,id,cancelled]{cancel_device_work(weak_queue(),*cancelled);transfers_.erase(id);});auto root=root_;auto weak=weak_queue();
    workers_.push_back(std::async(std::launch::async,[call,api,payload=std::move(payload),root,weak,cancelled,id,url]{
      try{CurlTransfer transfer{weak,call,api,id,Json::object(),{},cancelled,api=="wx.downloadFile"?33554432u:1048576u};CURL* raw=curl_easy_init();if(!raw)throw std::runtime_error("cannot initialize system HTTP transport");auto handle=std::unique_ptr<CURL,decltype(&curl_easy_cleanup)>(raw,curl_easy_cleanup);curl_easy_setopt(raw,CURLOPT_URL,url.c_str());curl_easy_setopt(raw,CURLOPT_PROTOCOLS_STR,"https");curl_easy_setopt(raw,CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(raw,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(raw,CURLOPT_TIMEOUT_MS,long(std::clamp(payload.value("timeout",60000),1,300000)));curl_easy_setopt(raw,CURLOPT_CONNECTTIMEOUT_MS,10000L);curl_easy_setopt(raw,CURLOPT_WRITEFUNCTION,curl_write);curl_easy_setopt(raw,CURLOPT_WRITEDATA,&transfer);curl_easy_setopt(raw,CURLOPT_HEADERFUNCTION,curl_header);curl_easy_setopt(raw,CURLOPT_HEADERDATA,&transfer);curl_easy_setopt(raw,CURLOPT_XFERINFOFUNCTION,curl_progress);curl_easy_setopt(raw,CURLOPT_XFERINFODATA,&transfer);curl_easy_setopt(raw,CURLOPT_NOPROGRESS,0L);
        curl_slist* headers=nullptr;auto supplied=payload.value("header",Json::object());for(auto entry=supplied.begin();entry!=supplied.end();++entry){auto value=entry.value().get<std::string>();if(entry.key().find_first_of("\r\n:")!=std::string::npos||value.find_first_of("\r\n")!=std::string::npos)throw std::invalid_argument("invalid HTTP header");headers=curl_slist_append(headers,(entry.key()+": "+value).c_str());}auto free_headers=std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)>(headers,curl_slist_free_all);curl_easy_setopt(raw,CURLOPT_HTTPHEADER,headers);
        std::string method=payload.value("method",api=="wx.uploadFile"?"POST":"GET"),body;std::vector<uint8_t> binary;std::unique_ptr<curl_mime,decltype(&curl_mime_free)> multipart(nullptr,curl_mime_free);
        if(api=="wx.uploadFile"){multipart.reset(curl_mime_init(raw));auto part=curl_mime_addpart(multipart.get());curl_mime_name(part,payload.at("name").get<std::string>().c_str());binary=managed_read(root,managed_path(root,payload.at("filePath").get<std::string>()),32*1024*1024);curl_mime_filename(part,"upload");curl_mime_data(part,reinterpret_cast<const char*>(binary.data()),binary.size());auto form=payload.value("formData",Json::object());for(auto field=form.begin();field!=form.end();++field){part=curl_mime_addpart(multipart.get());curl_mime_name(part,field.key().c_str());auto value=field.value().is_string()?field.value().get<std::string>():field.value().dump();curl_mime_data(part,value.data(),value.size());}curl_easy_setopt(raw,CURLOPT_MIMEPOST,multipart.get());}
        else if(payload.contains("data")){auto data=payload.at("data");if(data.is_object()&&data.contains("__weappBinary")){binary=binary_bytes(data);body.assign(binary.begin(),binary.end());}else body=data.is_string()?data.get<std::string>():data.dump();if(body.size()>1048576)throw std::invalid_argument("HTTP body exceeds one MiB limit");curl_easy_setopt(raw,CURLOPT_POSTFIELDS,body.data());curl_easy_setopt(raw,CURLOPT_POSTFIELDSIZE_LARGE,curl_off_t(body.size()));}
        curl_easy_setopt(raw,CURLOPT_CUSTOMREQUEST,method.c_str());auto code=curl_easy_perform(raw);if(code!=CURLE_OK)throw std::runtime_error(transfer.overflow?"native response byte budget exceeded":curl_easy_strerror(code));long status=0;curl_easy_getinfo(raw,CURLINFO_RESPONSE_CODE,&status);DeviceSideEffect publication(weak,cancelled.get());if(!publication)return;Json result={{"objectId",id},{"statusCode",status},{"header",transfer.headers},{"cookies",Json::array()}};
        if(api=="wx.downloadFile"){auto path=payload.value("filePath","wxfile://user/download-"+id);managed_write(root,managed_path(root,path),transfer.bytes);result["tempFilePath"]=path;result["filePath"]=path;}
        else if(payload.value("responseType","")=="arraybuffer")result["data"]=inline_binary(transfer.bytes);else{auto text=std::string(transfer.bytes.begin(),transfer.bytes.end());auto parsed=payload.value("dataType","json")=="json"?Json::parse(text,nullptr,false):Json(Json::value_t::discarded);result["data"]=parsed.is_discarded()?Json(text):parsed;}
        queue_complete(weak,call,device_ok(api,std::move(result)));
      }catch(const std::exception& error){DeviceSideEffect publication(weak,cancelled.get());if(publication)queue_failure(weak,call,device_failure(api,error.what()));}
    }));
  }
#endif
  void start_wifi_call(const std::string& call,const std::string& api){auto weak=weak_queue();auto generation=wifi_generation_;workers_.push_back(std::async(std::launch::async,[this,weak,call,api,generation]{try{auto result=native_wifi(api);queue_action(weak,[this,weak,call,api,generation,result]{auto queue=weak.lock();if(!queue||queue->closed||!calls_.count(call))return;if(generation!=wifi_generation_){fail(call,api,"Wi-Fi operation retired","cancelled");return;}wifi_active_=true;if(api=="wx.getWifiList"){queue_event(weak,"GetWifiList","",result);done(call,api);}else done(call,api,result);});}catch(const std::exception& error){queue_failure(weak,call,device_failure(api,error.what()));}}));}
  void release_screen(){if(screen_bus_&&screen_cookie_)bus_call(screen_bus_,"org.freedesktop.ScreenSaver","/org/freedesktop/ScreenSaver","org.freedesktop.ScreenSaver","UnInhibit",g_variant_new("(u)",screen_cookie_));screen_cookie_=0;screen_bus_.reset();}
  bool system_call(const std::string& call,const std::string& api,const Json& payload){
    if(api=="wx.startWifi"||api=="wx.getWifiList"||api=="wx.getConnectedWifi"){if(api=="wx.startWifi")start_wifi_call(call,api);else permit("scope.userLocation",api,call,[this,call,api]{start_wifi_call(call,api);});return true;}
    if(api=="wx.stopWifi"){wifi_active_=false;++wifi_generation_;done(call,api);return true;}
    if(api=="wx.openLocation"){double latitude=payload.at("latitude").get<double>(),longitude=payload.at("longitude").get<double>();if(!std::isfinite(latitude)||!std::isfinite(longitude)||fabs(latitude)>90||fabs(longitude)>180)throw std::invalid_argument("invalid map location");auto uri="geo:"+std::to_string(latitude)+","+std::to_string(longitude);GError* error=nullptr;if(!g_app_info_launch_default_for_uri(uri.c_str(),nullptr,&error)){auto message=std::string(error?error->message:"system map handler unavailable");if(error)g_error_free(error);throw std::runtime_error(message);}done(call,api);return true;}
    if(api=="wx.setKeepScreenOn"){if(!payload.value("keepScreenOn",false)){release_screen();done(call,api);return true;}if(screen_cookie_){done(call,api);return true;}auto bus=system_bus(G_BUS_TYPE_SESSION);auto result=bus_call(bus,"org.freedesktop.ScreenSaver","/org/freedesktop/ScreenSaver","org.freedesktop.ScreenSaver","Inhibit",g_variant_new("(ss)",encode(options_.application).value("appId","").c_str(),"Active application screen"),G_VARIANT_TYPE("(u)"));g_variant_get(result.get(),"(u)",&screen_cookie_);screen_bus_=bus;done(call,api);return true;}
    if(api=="wx.getScreenBrightness"||api=="wx.setScreenBrightness"){std::filesystem::path selected;for(const auto& entry:std::filesystem::directory_iterator("/sys/class/backlight")){if(std::filesystem::exists(entry.path()/"brightness")&&std::filesystem::exists(entry.path()/"max_brightness")){selected=entry.path();break;}}if(selected.empty())throw std::runtime_error("system has no controllable backlight");int maximum=0,current=0;std::ifstream max_file(selected/"max_brightness"),current_file(selected/"brightness");max_file>>maximum;current_file>>current;if(maximum<=0||current<0)throw std::runtime_error("system backlight information unavailable");if(api=="wx.getScreenBrightness")done(call,api,{{"value",double(current)/maximum}});else{double value=payload.at("value").get<double>();if(!std::isfinite(value)||value<0||value>1)throw std::invalid_argument("screen brightness must be between zero and one");std::ofstream output(selected/"brightness");if(!output)throw std::runtime_error("OS backlight write permission denied");output<<int(std::lround(value*maximum))<<"\n";output.flush();if(!output)throw std::runtime_error("OS backlight write failed");done(call,api);}return true;}
    return false;
  }
  bool platform_call(const std::string& call,const std::string& api,const Json& payload,const Json&) override {
    if(socket_call(call,api,payload)||system_call(call,api,payload))return true;
#if defined(WEAPP_DEVICE_CURL)
    if(api=="wx.request"||api=="wx.downloadFile"||api=="wx.uploadFile"){request(call,api,payload);return true;}
    if(api=="object:RequestTask.abort"||api=="object:DownloadTask.abort"||api=="object:UploadTask.abort"){auto found=transfers_.find(payload.value("objectId",""));if(found==transfers_.end())throw std::invalid_argument("native task is unavailable");cancel_device_work(weak_queue(),*found->second);done(call,api);return true;}
#endif
    if(api=="wx.getNetworkType"){done(call,api,{{"networkType",g_network_monitor_get_network_available(network_)?"unknown":"none"}});return true;}
    if(api=="wx.getBatteryInfo"){done(call,api,linux_battery());return true;}
    if(api=="wx.setClipboardData"){auto text=payload.at("data").get<std::string>();permit("scope.clipboard",api,call,[this,text,call,api]{auto clipboard=gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);if(!clipboard){fail(call,api,"system clipboard unavailable");return;}gtk_clipboard_set_text(clipboard,text.data(),int(text.size()));gtk_clipboard_store(clipboard);done(call,api);});return true;}
    if(api=="wx.getClipboardData"){permit("scope.clipboard",api,call,[this,call,api]{auto weak=weak_queue();auto clipboard=gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);if(!clipboard){fail(call,api,"system clipboard unavailable");return;}struct Read {std::weak_ptr<DeviceQueue> queue;std::string call,api;};auto read=new Read{weak,call,api};gtk_clipboard_request_text(clipboard,+[](GtkClipboard*,const gchar* text,gpointer pointer){std::unique_ptr<Read> data(static_cast<Read*>(pointer));queue_complete(data->queue,data->call,device_ok(data->api,{{"data",text?text:""}}));},read);});return true;}
    if(api=="wx.showModal"||api=="wx.showActionSheet"){std::vector<std::string> buttons;if(api=="wx.showModal"){buttons.push_back(payload.value("confirmText","OK"));if(payload.value("showCancel",true))buttons.push_back(payload.value("cancelText","Cancel"));}else{auto items=payload.at("itemList");if(!items.is_array()||items.empty()||items.size()>6)throw std::invalid_argument("action sheet needs one to six items");for(auto& item:items)buttons.push_back(item.get<std::string>());buttons.push_back("Cancel");}auto weak=weak_queue();auto count=buttons.size();dialog(payload.value("title",""),payload.value("content",""),buttons,[weak,call,api,count](int response){if(api=="wx.showModal")queue_complete(weak,call,device_ok(api,{{"confirm",response==0},{"cancel",response!=0}}));else if(response>=0&&size_t(response)<count-1)queue_complete(weak,call,device_ok(api,{{"tapIndex",response}}));else queue_failure(weak,call,device_failure(api,"user cancelled"));});return true;}
    return false;
  }
  void permissions_reset() override {++wifi_generation_;wifi_active_=false;}
  void retire_platform() override {++wifi_generation_;release_screen();if(network_signal_){g_signal_handler_disconnect(network_,network_signal_);network_signal_=0;}auto dialogs=std::move(dialogs_);for(auto widget:dialogs)gtk_widget_destroy(widget);for(auto& transfer:transfers_)cancel_device_work(weak_queue(),*transfer.second);transfers_.clear();}
 public:
  LinuxDeviceApis(const DeviceApiOptions& options,Command command):DeviceSockets(options,std::move(command)) {
    network_=g_network_monitor_get_default();
    for(auto api:{"wx.startWifi","wx.stopWifi","wx.getWifiList","wx.getConnectedWifi","wx.openLocation","wx.setKeepScreenOn","wx.getScreenBrightness","wx.setScreenBrightness","wx.getNetworkType","wx.getBatteryInfo","wx.getClipboardData","wx.setClipboardData","wx.showModal","wx.showActionSheet"})add(api);
    add_event("wx.onGetWifiList","wx.offGetWifiList","GetWifiList");
    add_event("wx.onNetworkStatusChange","wx.offNetworkStatusChange","NetworkStatusChange");auto weak=new std::weak_ptr<DeviceQueue>(weak_queue());network_signal_=g_signal_connect_data(network_,"network-changed",G_CALLBACK(+[](GNetworkMonitor*,gboolean available,gpointer pointer){queue_event(*static_cast<std::weak_ptr<DeviceQueue>*>(pointer),"NetworkStatusChange","",{{"isConnected",bool(available)},{"networkType",available?"unknown":"none"}});}),weak,[](gpointer pointer,GClosure*){delete static_cast<std::weak_ptr<DeviceQueue>*>(pointer);},GConnectFlags(0));
#if defined(WEAPP_DEVICE_CURL)
    static const auto curl_ready=curl_global_init(CURL_GLOBAL_DEFAULT);if(curl_ready!=CURLE_OK)throw std::runtime_error("system HTTP initialization failed");
#if defined(WEAPP_DEVICE_CURL_WEBSOCKET)
    if(curl_socket_available())install_socket_runner(run_curl_socket);
#endif
    for(auto api:{"wx.request","wx.downloadFile","wx.uploadFile","object:RequestTask.abort","object:DownloadTask.abort","object:UploadTask.abort"})add(api);
    for(auto type:{"RequestTask","DownloadTask","UploadTask"})add_event("object:"+std::string(type)+".onHeadersReceived","object:"+std::string(type)+".offHeadersReceived","HeadersReceived");
    add_event("object:RequestTask.onChunkReceived","object:RequestTask.offChunkReceived","ChunkReceived");for(auto type:{"DownloadTask","UploadTask"})add_event("object:"+std::string(type)+".onProgressUpdate","object:"+std::string(type)+".offProgressUpdate","ProgressUpdate");
#endif
  }
  ~LinuxDeviceApis() override {retire();}
};
}
namespace weapp::desktop {
std::unique_ptr<DeviceApis> DeviceApis::create(const DeviceApiOptions& options,Command command,Status& status) {
  try {auto result=std::make_unique<detail::LinuxDeviceApis>(options,std::move(command));status={};return result;}
  catch(const std::exception& error){status=Status::error("ERR_NATIVE_DEVICES",error.what());return {};}
}
}
