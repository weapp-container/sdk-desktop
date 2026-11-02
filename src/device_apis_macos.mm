#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreLocation/CoreLocation.h>
#import <CoreImage/CoreImage.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreWLAN/CoreWLAN.h>
#import <IOKit/ps/IOPowerSources.h>
#import <IOKit/ps/IOPSKeys.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <SystemConfiguration/SystemConfiguration.h>
#import <Vision/Vision.h>
#import <CoreBluetooth/CoreBluetooth.h>
#import <Contacts/Contacts.h>
#import <EventKit/EventKit.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <objc/runtime.h>
#include "device_location_common.hpp"
#include <cmath>

using namespace weapp::desktop;
using namespace weapp::desktop::detail;
namespace {
NSString* ns(const std::string& value) {return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];}
std::string utf(NSString* value) {return value.UTF8String ?: "";}
CBUUID* ble_uuid(const std::string& value) {
  if(value.size()!=4&&value.size()!=8&&value.size()!=36)throw std::invalid_argument("invalid Bluetooth UUID");
  for(size_t index=0;index<value.size();++index){bool separator=value.size()==36&&(index==8||index==13||index==18||index==23);if(separator?value[index]!='-':!std::isxdigit(static_cast<unsigned char>(value[index])))throw std::invalid_argument("invalid Bluetooth UUID");}
  return [CBUUID UUIDWithString:ns(value)];
}
std::string origin(NSURL* url) {
  auto scheme=utf(url.scheme.lowercaseString),host=utf(url.host.lowercaseString);
  if((scheme!="https"&&scheme!="wss")||host.empty()||url.user||url.password)throw std::invalid_argument("native networking requires a credential-free HTTPS/WSS URL");
  if(host.find(':')!=std::string::npos)host="["+host+"]";
  return (scheme=="wss"?"https":scheme)+std::string("://")+host+(url.port&&url.port.intValue!=443?":"+std::to_string(url.port.intValue):"");
}
struct Transfer {
  std::weak_ptr<DeviceQueue> queue;std::string call,api,object;Json payload;std::filesystem::path root;
  std::vector<uint8_t> bytes;Json headers=Json::object();int64_t status=0;std::string mime;bool too_large=false;
  std::shared_ptr<std::atomic<bool>> cancelled=std::make_shared<std::atomic<bool>>(false);
};
struct NetworkState {
  std::mutex mutex;std::map<NSUInteger,std::shared_ptr<Transfer>> transfers;std::vector<std::string> origins;
};
}
@interface WCDeviceNetwork : NSObject<NSURLSessionDataDelegate,NSURLSessionTaskDelegate,NSURLSessionWebSocketDelegate> {
 @public std::shared_ptr<NetworkState> state;
 @public std::weak_ptr<DeviceQueue> queue;
 @public std::map<NSUInteger,std::string> socketObjects;
}
@end
@implementation WCDeviceNetwork
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task willPerformHTTPRedirection:(NSHTTPURLResponse*)response newRequest:(NSURLRequest*)request completionHandler:(void (^)(NSURLRequest*))completion {
  try {auto candidate=origin(request.URL);completion(std::find(state->origins.begin(),state->origins.end(),candidate)!=state->origins.end()?request:nil);}catch(...){completion(nil);}
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didReceiveChallenge:(NSURLAuthenticationChallenge*)challenge completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition,NSURLCredential*))completion {
  completion(NSURLSessionAuthChallengePerformDefaultHandling,nil);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveResponse:(NSURLResponse*)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
  std::shared_ptr<Transfer> transfer;{std::lock_guard<std::mutex> lock(state->mutex);auto found=state->transfers.find(task.taskIdentifier);if(found!=state->transfers.end())transfer=found->second;}
  if(!transfer){completion(NSURLSessionResponseCancel);return;}
  if([response isKindOfClass:NSHTTPURLResponse.class]){auto http=(NSHTTPURLResponse*)response;transfer->status=http.statusCode;for(id key in http.allHeaderFields)transfer->headers[utf([key description])]=utf([http.allHeaderFields[key] description]);}
  auto limit=transfer->api=="wx.downloadFile"?33554432LL:1048576LL;
  if(response.expectedContentLength>limit){transfer->too_large=true;completion(NSURLSessionResponseCancel);return;}
  transfer->mime=utf(response.MIMEType);queue_event(transfer->queue,"HeadersReceived",transfer->object,{{"header",transfer->headers},{"statusCode",transfer->status}});completion(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
  std::shared_ptr<Transfer> transfer;{std::lock_guard<std::mutex> lock(state->mutex);auto found=state->transfers.find(task.taskIdentifier);if(found!=state->transfers.end())transfer=found->second;}
  if(!transfer)return;size_t limit=transfer->api=="wx.downloadFile"?33554432:1048576;
  if(data.length>limit-transfer->bytes.size()){transfer->too_large=true;[task cancel];return;}
  auto begin=(const uint8_t*)data.bytes;transfer->bytes.insert(transfer->bytes.end(),begin,begin+data.length);
  for(size_t offset=0;offset<data.length;offset+=1048576){auto count=std::min<size_t>(1048576,data.length-offset);queue_event(transfer->queue,"ChunkReceived",transfer->object,{{"data",inline_binary(std::vector<uint8_t>(begin+offset,begin+offset+count))}});}
  queue_event(transfer->queue,"ProgressUpdate",transfer->object,{{"totalBytesWritten",transfer->bytes.size()},{"totalBytesExpectedToWrite",task.response.expectedContentLength},{"progress",task.response.expectedContentLength>0?double(transfer->bytes.size())/task.response.expectedContentLength*100.0:0.0}});
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
  std::shared_ptr<Transfer> transfer;{std::lock_guard<std::mutex> lock(state->mutex);auto found=state->transfers.find(task.taskIdentifier);if(found!=state->transfers.end()){transfer=found->second;state->transfers.erase(found);}}
  if(!transfer)return;
  DeviceSideEffect publication(transfer->queue,transfer->cancelled.get());if(!publication)return;
  if(error||transfer->too_large){queue_failure(transfer->queue,transfer->call,device_failure(transfer->api,transfer->too_large?"response exceeds native byte budget":utf(error.localizedDescription),"invalid_request",error.code));return;}
  try {
    Json result={{"objectId",transfer->object},{"statusCode",transfer->status},{"header",transfer->headers},{"cookies",Json::array()}};
    if(transfer->api=="wx.downloadFile") {
      auto relative=transfer->payload.value("filePath",std::string("wxfile://user/download-")+utf(NSUUID.UUID.UUIDString));
      managed_write(transfer->root,managed_path(transfer->root,relative),transfer->bytes);result["tempFilePath"]=relative;result["filePath"]=relative;
    }else if(transfer->payload.value("responseType","")=="arraybuffer")result["data"]=inline_binary(transfer->bytes);
    else {auto text=std::string(transfer->bytes.begin(),transfer->bytes.end());
      if(transfer->api=="wx.request"&&transfer->payload.value("dataType","json")=="json") {auto parsed=Json::parse(text,nullptr,false);result["data"]=parsed.is_discarded()?Json(text):parsed;}
      else result["data"]=text;}
    queue_complete(transfer->queue,transfer->call,device_ok(transfer->api,std::move(result)));
  }catch(const std::exception& exception){queue_failure(transfer->queue,transfer->call,device_failure(transfer->api,exception.what()));}
}
- (void)URLSession:(NSURLSession*)session webSocketTask:(NSURLSessionWebSocketTask*)task didOpenWithProtocol:(NSString*)protocol {
  std::string object;{std::lock_guard<std::mutex> lock(state->mutex);auto found=socketObjects.find(task.taskIdentifier);if(found!=socketObjects.end())object=found->second;}
  if(!object.empty())queue_event(queue,"Open",object,{{"header",Json::object()},{"protocol",utf(protocol)}});
}
- (void)URLSession:(NSURLSession*)session webSocketTask:(NSURLSessionWebSocketTask*)task didCloseWithCode:(NSURLSessionWebSocketCloseCode)code reason:(NSData*)reason {
  std::string object;{std::lock_guard<std::mutex> lock(state->mutex);auto found=socketObjects.find(task.taskIdentifier);if(found!=socketObjects.end()){object=found->second;socketObjects.erase(found);}}
  if(!object.empty())queue_event(queue,"Close",object,{{"code",(int64_t)code},{"reason",utf([[NSString alloc]initWithData:reason encoding:NSUTF8StringEncoding])}});
}
@end

@interface WCDeviceLocation : NSObject<CLLocationManagerDelegate> {
 @public std::function<void(CLLocation*,NSError*)> update;
 @public std::function<void(CLAuthorizationStatus)> authorization;
}
@end
@implementation WCDeviceLocation
- (void)locationManager:(CLLocationManager*)manager didUpdateLocations:(NSArray<CLLocation*>*)locations {if(update&&locations.lastObject)update(locations.lastObject,nil);}
- (void)locationManager:(CLLocationManager*)manager didFailWithError:(NSError*)error {if(update)update(nil,error);}
- (void)locationManagerDidChangeAuthorization:(CLLocationManager*)manager {if(authorization)authorization(manager.authorizationStatus);}
@end

namespace {
struct MediaMailbox {
  std::mutex mutex;
  std::map<std::string,Json> targets,latest;
  std::map<std::string,uint64_t> sequences,incarnations;
};
void media_pixels(const std::weak_ptr<DeviceQueue>& weak,const std::shared_ptr<MediaMailbox>& mailbox,const std::string& node,uint64_t incarnation,CVPixelBufferRef buffer,CIContext* context) {
  auto queue=weak.lock();if(!queue||queue->closed||!buffer)return;
  CIImage* image=[CIImage imageWithCVPixelBuffer:buffer];CGRect extent=image.extent;
  if(extent.size.width<=0||extent.size.height<=0)return;
  double scale=std::min(1.0,512.0/std::max(extent.size.width,extent.size.height));
  uint32_t width=std::max(1u,uint32_t(extent.size.width*scale)),height=std::max(1u,uint32_t(extent.size.height*scale));
  image=[image imageByApplyingTransform:CGAffineTransformMakeScale(scale,scale)];
  std::vector<uint8_t> pixels(size_t(width)*height*4);CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  [context render:image toBitmap:pixels.data() rowBytes:width*4 bounds:CGRectMake(0,0,width,height) format:kCIFormatRGBA8 colorSpace:color];CGColorSpaceRelease(color);
  std::lock_guard<std::mutex> lock(mailbox->mutex);auto found=mailbox->targets.find(node);if(found==mailbox->targets.end()||mailbox->incarnations[node]!=incarnation)return;
  const auto& target=found->second;Json data={{"sessionId",target.at("sessionId")},{"accountEpoch",target.at("accountEpoch")},{"pageId",target.at("pageId")},{"nodeId",node},{"sequence",std::to_string(++mailbox->sequences[node])},{"width",width},{"height",height},{"format","rgba8"},{"pixels",std::move(pixels)}};
  mailbox->latest[node]=std::move(data);
}
}
@interface WCDeviceFrames : NSObject<AVCaptureVideoDataOutputSampleBufferDelegate> {
 @public std::weak_ptr<DeviceQueue> queue;
 @public std::shared_ptr<MediaMailbox> mailbox;
 @public std::string node;
 @public uint64_t incarnation;
 @public CIContext* context;
}
@end
@implementation WCDeviceFrames
- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample fromConnection:(AVCaptureConnection*)connection {media_pixels(queue,mailbox,node,incarnation,CMSampleBufferGetImageBuffer(sample),context);}
@end

@interface WCDevicePhoto : NSObject<AVCapturePhotoCaptureDelegate> {
 @public std::weak_ptr<DeviceQueue> queue;
 @public std::string call,api,path;
 @public std::filesystem::path root;
 @public std::shared_ptr<std::atomic<bool>> cancelled,finished;
}
@end
@implementation WCDevicePhoto
- (void)captureOutput:(AVCapturePhotoOutput*)output didFinishProcessingPhoto:(AVCapturePhoto*)photo error:(NSError*)error {
  auto completion=std::shared_ptr<void>(nullptr,[done=finished](void*){if(done)*done=true;});if(cancelled&&cancelled->load())return;
  if(error){queue_failure(queue,call,device_failure(api,utf(error.localizedDescription),"invalid_request",error.code));return;}
  try{NSData* data=photo.fileDataRepresentation;if(!data||data.length>32*1024*1024)throw std::runtime_error("captured photo exceeds native file budget");
    DeviceSideEffect publication(queue,cancelled.get());if(!publication)return;auto begin=(const uint8_t*)data.bytes;managed_write(root,managed_path(root,path),std::vector<uint8_t>(begin,begin+data.length));queue_complete(queue,call,device_ok(api,{{"tempImagePath",path}}));
  }catch(const std::exception& exception){queue_failure(queue,call,device_failure(api,exception.what()));}
}
@end

// CoreBluetooth callbacks only enqueue owner actions. Closing a driver clears
// delegate closures before releasing managers, including late OS callbacks.
@interface WCMacBluetooth : NSObject<CBCentralManagerDelegate,CBPeripheralDelegate> {
 @public std::function<void()> stateChanged;
 @public std::function<void(CBPeripheral*,NSDictionary*,NSNumber*)> discovered;
 @public std::function<void(const std::string&,CBPeripheral*,CBService*,CBCharacteristic*,NSError*)> completed;
}
@end
@implementation WCMacBluetooth
- (void)centralManagerDidUpdateState:(CBCentralManager*)central {if(stateChanged)stateChanged();}
- (void)centralManager:(CBCentralManager*)central didDiscoverPeripheral:(CBPeripheral*)peripheral advertisementData:(NSDictionary*)data RSSI:(NSNumber*)rssi {if(discovered)discovered(peripheral,data,rssi);}
- (void)centralManager:(CBCentralManager*)central didConnectPeripheral:(CBPeripheral*)peripheral {if(completed)completed("connect",peripheral,nil,nil,nil);}
- (void)centralManager:(CBCentralManager*)central didFailToConnectPeripheral:(CBPeripheral*)peripheral error:(NSError*)error {if(completed)completed("connect",peripheral,nil,nil,error ?: [NSError errorWithDomain:CBErrorDomain code:CBErrorConnectionFailed userInfo:nil]);}
- (void)centralManager:(CBCentralManager*)central didDisconnectPeripheral:(CBPeripheral*)peripheral error:(NSError*)error {if(completed)completed("disconnect",peripheral,nil,nil,error);}
- (void)peripheral:(CBPeripheral*)peripheral didDiscoverServices:(NSError*)error {if(completed)completed("services",peripheral,nil,nil,error);}
- (void)peripheral:(CBPeripheral*)peripheral didDiscoverCharacteristicsForService:(CBService*)service error:(NSError*)error {if(completed)completed("characteristics",peripheral,service,nil,error);}
- (void)peripheral:(CBPeripheral*)peripheral didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic error:(NSError*)error {if(completed)completed("read",peripheral,characteristic.service,characteristic,error);}
- (void)peripheral:(CBPeripheral*)peripheral didWriteValueForCharacteristic:(CBCharacteristic*)characteristic error:(NSError*)error {if(completed)completed("write",peripheral,characteristic.service,characteristic,error);}
- (void)peripheral:(CBPeripheral*)peripheral didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic error:(NSError*)error {if(completed)completed("notify",peripheral,characteristic.service,characteristic,error);}
- (void)peripheral:(CBPeripheral*)peripheral didReadRSSI:(NSNumber*)RSSI error:(NSError*)error {if(!error)objc_setAssociatedObject(peripheral,@selector(peripheral:didReadRSSI:error:),RSSI,OBJC_ASSOCIATION_RETAIN_NONATOMIC);if(completed)completed("rssi",peripheral,nil,nil,error);}
@end
@interface WCMacRecorder : NSObject<AVAudioRecorderDelegate> {
 @public std::function<void(bool,NSError*)> finished;
}
@end
@implementation WCMacRecorder
- (void)audioRecorderDidFinishRecording:(AVAudioRecorder*)recorder successfully:(BOOL)success {if(finished)finished(success,nil);}
- (void)audioRecorderEncodeErrorDidOccur:(AVAudioRecorder*)recorder error:(NSError*)error {if(finished)finished(false,error);}
@end

namespace weapp::desktop::detail {
class MacDeviceApis final : public DeviceBase {
  WCDeviceNetwork* network_;NSURLSession* session_;std::map<std::string,NSURLSessionTask*> tasks_;std::map<std::string,std::shared_ptr<std::atomic<bool>>> network_cancellations_;
  WCDeviceLocation* location_delegate_=nil;CLLocationManager* location_=nil;
  struct MacLocationRequest {LocationOptions options;std::shared_ptr<std::atomic<bool>> delivery;std::chrono::steady_clock::time_point deadline;};
  std::map<std::string,MacLocationRequest> location_calls_;
  struct MacLocationIntent {std::shared_ptr<std::atomic<bool>> delivery;bool continuous;};
  std::map<std::string,MacLocationIntent> location_intents_;
  bool continuous_location_=false,location_started_=false;uint64_t location_generation_=0;
  LocationOptions location_watch_options_;
  std::shared_ptr<std::atomic<bool>> location_watch_delivery_,location_terminal_delivery_,location_overflow_;
  struct Audio {AVPlayer* player=nil;id timer=nil,ended=nil;Json properties=Json::object();};std::map<std::string,Audio> audio_;
  struct Mount {AVCaptureSession* capture=nil;AVCapturePhotoOutput* photo=nil;AVCaptureVideoDataOutput* output=nil;WCDeviceFrames* capture_delegate=nil;AVPlayer* player=nil;AVPlayerItemVideoOutput* video_output=nil;Json snapshot;std::string source;uint64_t sequence=0,incarnation=0;bool permission_pending=false,permission_denied=false;};std::map<std::string,Mount> mounts_;
  std::shared_ptr<MediaMailbox> media_=std::make_shared<MediaMailbox>();CIContext* media_context_=[CIContext contextWithOptions:nil];uint64_t mount_sequence_=0;
  std::map<std::string,std::string> camera_nodes_,video_nodes_;std::vector<WCDevicePhoto*> photos_;
  std::vector<NSWindow*> dialogs_;IOPMAssertionID wake_=kIOPMNullAssertionID;
  WCMacBluetooth* bluetooth_delegate_=nil;CBCentralManager* bluetooth_=nil;std::string bluetooth_open_;uint64_t bluetooth_generation_=0;
  std::shared_ptr<std::atomic<bool>> bluetooth_delivery_,bluetooth_discovery_delivery_,bluetooth_terminal_delivery_,bluetooth_overflow_;
  std::map<std::string,std::shared_ptr<std::atomic<bool>>> bluetooth_open_intents_;
  std::map<std::string,CBPeripheral*> peripherals_;std::map<std::string,Json> bluetooth_devices_;
  struct BlePending {std::string call,api,operation,service,characteristic;std::chrono::steady_clock::time_point deadline;};std::map<std::string,BlePending> ble_pending_;
  AVAudioRecorder* recorder_=nil;WCMacRecorder* recorder_delegate_=nil;std::string recorder_object_,recorder_path_;std::filesystem::path recorder_temporary_;double recorder_duration_=0;uint64_t recorder_generation_=0;
  CNContactStore* contacts_=[CNContactStore new];EKEventStore* calendar_=[EKEventStore new];std::vector<LAContext*> authentication_;
  static std::string peripheral_id(CBPeripheral* peripheral){return utf(peripheral.identifier.UUIDString);}
  void bluetooth_event(const std::string& name,Json value,std::shared_ptr<std::atomic<bool>> token={}) {
    DeviceQueued item{DeviceQueued::NativeEvent,"",name,"",std::move(value)};item.delivery_allowed=token?token:bluetooth_delivery_;
    if(!queue_->push(std::move(item))&&bluetooth_overflow_)*bluetooth_overflow_=true;
  }
  void bluetooth_receipt(const std::string& call,const std::string& api,const std::shared_ptr<std::atomic<bool>>& token) {
    DeviceQueued item{DeviceQueued::Complete,call,"","",device_ok(api)};item.delivery_allowed=token;
    if(!queue_->push(std::move(item))&&bluetooth_overflow_)*bluetooth_overflow_=true;
  }
  void finish_ble(const std::string& kind,CBPeripheral* peripheral,CBService* service,CBCharacteristic* characteristic,NSError* error) {
    auto id=peripheral_id(peripheral);
    if(kind=="connect"||kind=="disconnect")bluetooth_event("BLEConnectionStateChange",{{"deviceId",id},{"connected",kind=="connect"&&!error}});
    if(kind=="read"&&!error&&characteristic.value.length<=1048576){auto start=(const uint8_t*)characteristic.value.bytes;bluetooth_event("BLECharacteristicValueChange",{{"deviceId",id},{"serviceId",utf(characteristic.service.UUID.UUIDString)},{"characteristicId",utf(characteristic.UUID.UUIDString)},{"value",inline_binary(std::vector<uint8_t>(start,start+characteristic.value.length))}});}
    auto found=ble_pending_.find(id);if(found==ble_pending_.end())return;auto pending=found->second;
    if(pending.operation!=kind&&kind!="disconnect")return;
    if(service&&!pending.service.empty()&&![service.UUID isEqual:ble_uuid(pending.service)])return;
    if(characteristic&&!pending.characteristic.empty()&&![characteristic.UUID isEqual:ble_uuid(pending.characteristic)])return;
    ble_pending_.erase(found);if(!calls_.count(pending.call))return;
    if(error||kind=="disconnect"){fail(pending.call,pending.api,error?utf(error.localizedDescription):"Bluetooth device disconnected","invalid_request",error.code);return;}
    Json value=Json::object();
    if(kind=="services"){value["services"]=Json::array();for(CBService* item in peripheral.services){if(value["services"].size()>=256)break;value["services"].push_back({{"uuid",utf(item.UUID.UUIDString)},{"isPrimary",bool(item.isPrimary)}});}}
    if(kind=="characteristics"){value["characteristics"]=Json::array();for(CBCharacteristic* item in service.characteristics){if(value["characteristics"].size()>=256)break;auto flags=item.properties;value["characteristics"].push_back({{"uuid",utf(item.UUID.UUIDString)},{"properties",{{"read",bool(flags&CBCharacteristicPropertyRead)},{"write",bool(flags&(CBCharacteristicPropertyWrite|CBCharacteristicPropertyWriteWithoutResponse))},{"notify",bool(flags&CBCharacteristicPropertyNotify)},{"indicate",bool(flags&CBCharacteristicPropertyIndicate)}}}});}}
    if(kind=="rssi"){NSNumber* rssi=objc_getAssociatedObject(peripheral,@selector(peripheral:didReadRSSI:error:));value["RSSI"]=rssi.intValue;}
    done(pending.call,pending.api,std::move(value));
  }
  void close_bluetooth() {
    ++bluetooth_generation_;
    if(bluetooth_delivery_)*bluetooth_delivery_=false;
    if(bluetooth_discovery_delivery_)*bluetooth_discovery_delivery_=false;
    if(bluetooth_terminal_delivery_)*bluetooth_terminal_delivery_=false;
    for(const auto& [call,intent]:bluetooth_open_intents_){*intent=false;if(!queue_->closed&&calls_.count(call))fail(call,"wx.openBluetoothAdapter","Bluetooth adapter closed","cancelled");}bluetooth_open_intents_.clear();
    for(auto& pending:ble_pending_)if(calls_.count(pending.second.call))fail(pending.second.call,pending.second.api,"Bluetooth adapter closed","cancelled");ble_pending_.clear();
    if(!bluetooth_open_.empty()&&calls_.count(bluetooth_open_))fail(bluetooth_open_,"wx.openBluetoothAdapter","Bluetooth adapter closed","cancelled");bluetooth_open_.clear();
    if(bluetooth_delegate_){bluetooth_delegate_->stateChanged={};bluetooth_delegate_->discovered={};bluetooth_delegate_->completed={};}
    [bluetooth_ stopScan];for(auto& entry:peripherals_){entry.second.delegate=nil;[bluetooth_ cancelPeripheralConnection:entry.second];}bluetooth_.delegate=nil;bluetooth_=nil;bluetooth_delegate_=nil;peripherals_.clear();bluetooth_devices_.clear();
  }
  void open_bluetooth(const std::string& call,const std::string& api,const std::shared_ptr<std::atomic<bool>>& intent) {
    if(!intent->load()||!foreground_){fail(call,api,"Bluetooth request was retired","cancelled");return;}
    if(bluetooth_){if(bluetooth_.state==CBManagerStatePoweredOn)bluetooth_receipt(call,api,intent);else fail(call,api,"Bluetooth adapter is not powered on");return;}
    if(![NSBundle.mainBundle objectForInfoDictionaryKey:@"NSBluetoothAlwaysUsageDescription"]){fail(call,api,"host Info.plist must declare NSBluetoothAlwaysUsageDescription","permission_denied");return;}
    if(bluetooth_terminal_delivery_)*bluetooth_terminal_delivery_=false;
    bluetooth_delivery_=intent;bluetooth_overflow_=std::make_shared<std::atomic<bool>>(false);auto overflow=bluetooth_overflow_;
    bluetooth_open_=call;bluetooth_delegate_=[WCMacBluetooth new];auto weak=weak_queue();auto generation=++bluetooth_generation_;
    bluetooth_delegate_->stateChanged=[this,weak,generation,overflow]{if(!queue_action(weak,[this,weak,generation]{auto queue=weak.lock();if(!queue||queue->closed||!bluetooth_||generation!=bluetooth_generation_)return;bool available=bluetooth_.state==CBManagerStatePoweredOn;bluetooth_event("BluetoothAdapterStateChange",{{"available",available},{"discovering",bool(bluetooth_.isScanning)}});if(!bluetooth_open_.empty()&&bluetooth_.state!=CBManagerStateUnknown&&bluetooth_.state!=CBManagerStateResetting){auto call=std::move(bluetooth_open_);bluetooth_open_.clear();if(calls_.count(call)){if(available)bluetooth_receipt(call,"wx.openBluetoothAdapter",bluetooth_delivery_);else fail(call,"wx.openBluetoothAdapter","OS Bluetooth unavailable or permission denied",bluetooth_.state==CBManagerStateUnauthorized?"permission_denied":"invalid_request");}}}))*overflow=true;};
    bluetooth_delegate_->discovered=[this,weak,generation,overflow](CBPeripheral* peripheral,NSDictionary* data,NSNumber* rssi){auto scan=bluetooth_discovery_delivery_;if(!queue_action(weak,[this,weak,generation,peripheral,data,rssi,scan]{auto queue=weak.lock();if(!queue||queue->closed||!bluetooth_||generation!=bluetooth_generation_||!scan||!scan->load())return;auto id=peripheral_id(peripheral);if(peripherals_.size()>=256&&!peripherals_.count(id))return;peripherals_[id]=peripheral;peripheral.delegate=bluetooth_delegate_;Json value={{"deviceId",id},{"name",utf(peripheral.name)},{"localName",utf(data[CBAdvertisementDataLocalNameKey])},{"RSSI",rssi.intValue},{"connectable",[data[CBAdvertisementDataIsConnectable]boolValue]},{"advertisServiceUUIDs",Json::array()}};for(CBUUID* service in data[CBAdvertisementDataServiceUUIDsKey])if(value["advertisServiceUUIDs"].size()<64)value["advertisServiceUUIDs"].push_back(utf(service.UUIDString));NSData* manufacturer=data[CBAdvertisementDataManufacturerDataKey];if(manufacturer.length&&manufacturer.length<=1048576){auto start=(const uint8_t*)manufacturer.bytes;value["advertisData"]=inline_binary(std::vector<uint8_t>(start,start+manufacturer.length));}bluetooth_devices_[id]=value;bluetooth_event("BluetoothDeviceFound",{{"devices",Json::array({value})}},scan);}))*overflow=true;};
    bluetooth_delegate_->completed=[this,weak,generation,overflow](const std::string& kind,CBPeripheral* peripheral,CBService* service,CBCharacteristic* characteristic,NSError* error){if(!queue_action(weak,[this,weak,generation,kind,peripheral,service,characteristic,error]{auto queue=weak.lock();if(queue&&!queue->closed&&bluetooth_&&generation==bluetooth_generation_)finish_ble(kind,peripheral,service,characteristic,error);}))*overflow=true;};
    bluetooth_=[[CBCentralManager alloc]initWithDelegate:bluetooth_delegate_ queue:dispatch_get_main_queue() options:@{CBCentralManagerOptionShowPowerAlertKey:@NO}];calls_.at(call).cancel=[this,call,intent]{*intent=false;if(bluetooth_open_==call)close_bluetooth();};
  }
  bool bluetooth_call(const std::string& call,const std::string& api,const Json& payload) {
    if(api=="wx.openBluetoothAdapter"){if(!foreground_)throw std::invalid_argument("Bluetooth requires a foreground application");auto intent=std::make_shared<std::atomic<bool>>(true);bluetooth_open_intents_[call]=intent;calls_.at(call).cancel=[intent]{*intent=false;};permit("scope.bluetooth",api,call,[this,call,api,intent]{open_bluetooth(call,api,intent);});return true;}
    if(api=="wx.closeBluetoothAdapter"){close_bluetooth();done(call,api);return true;}
    if(api=="wx.openSystemBluetoothSetting"){if(![NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.BluetoothSettings"]])throw std::runtime_error("cannot open system Bluetooth settings");done(call,api);return true;}
    if(api.find("Bluetooth")==std::string::npos&&api.find("BLE")==std::string::npos)return false;
    if(!bluetooth_||bluetooth_.state!=CBManagerStatePoweredOn)throw std::runtime_error("Bluetooth adapter is not open and available");
    if(api=="wx.getBluetoothAdapterState"){done(call,api,{{"available",true},{"discovering",bool(bluetooth_.isScanning)}});return true;}
    if(api=="wx.startBluetoothDevicesDiscovery"){if(!foreground_)throw std::invalid_argument("Bluetooth scanning requires a foreground application");NSMutableArray<CBUUID*>* services=[NSMutableArray array];auto filters=payload.value("services",Json::array());if(!filters.is_array()||filters.size()>16)throw std::invalid_argument("Bluetooth service filter limit exceeded");for(const auto& filter:filters)[services addObject:ble_uuid(filter.get<std::string>())];if(bluetooth_discovery_delivery_)*bluetooth_discovery_delivery_=false;bluetooth_discovery_delivery_=std::make_shared<std::atomic<bool>>(true);[bluetooth_ scanForPeripheralsWithServices:services.count?services:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey:@(payload.value("allowDuplicatesKey",false))}];done(call,api);return true;}
    if(api=="wx.stopBluetoothDevicesDiscovery"){if(bluetooth_discovery_delivery_)*bluetooth_discovery_delivery_=false;[bluetooth_ stopScan];done(call,api);return true;}
    if(api=="wx.getBluetoothDevices"||api=="wx.getConnectedBluetoothDevices"){Json devices=Json::array();for(const auto& item:bluetooth_devices_){auto peripheral=peripherals_.at(item.first);if(api=="wx.getConnectedBluetoothDevices"&&peripheral.state!=CBPeripheralStateConnected)continue;auto filter=payload.value("services",Json::array());bool matches=filter.empty();for(const auto& expected:filter)for(CBService* service in peripheral.services)if([service.UUID isEqual:ble_uuid(expected.get<std::string>())])matches=true;if(matches)devices.push_back(item.second);}done(call,api,{{"devices",devices}});return true;}
    auto id=payload.at("deviceId").get<std::string>();auto selected=peripherals_.find(id);if(selected==peripherals_.end())throw std::invalid_argument("Bluetooth device was not discovered by this application");auto peripheral=selected->second;
    if(api=="wx.closeBLEConnection"){[bluetooth_ cancelPeripheralConnection:peripheral];done(call,api);return true;}
    if(api=="wx.getBLEMTU"){if(peripheral.state!=CBPeripheralStateConnected)throw std::runtime_error("Bluetooth device is disconnected");done(call,api,{{"mtu",[peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse]+3}});return true;}
    if(ble_pending_.count(id))throw std::invalid_argument("one Bluetooth operation per device may run at a time");
    auto begin=[&](const std::string& operation,CBService* service=nil,CBCharacteristic* characteristic=nil){ble_pending_[id]={call,api,operation,service?utf(service.UUID.UUIDString):"",characteristic?utf(characteristic.UUID.UUIDString):"",std::chrono::steady_clock::now()+std::chrono::milliseconds(std::clamp(payload.value("timeout",10000),100,60000))};calls_.at(call).cancel=[this,id,call]{auto pending=ble_pending_.find(id);if(pending!=ble_pending_.end()&&pending->second.call==call){if(pending->second.operation=="connect")[bluetooth_ cancelPeripheralConnection:peripherals_.at(id)];ble_pending_.erase(pending);}};};
    if(api=="wx.createBLEConnection"){if(peripheral.state==CBPeripheralStateConnected){done(call,api);return true;}begin("connect");[bluetooth_ connectPeripheral:peripheral options:nil];return true;}
    if(peripheral.state!=CBPeripheralStateConnected)throw std::runtime_error("Bluetooth device is disconnected");
    if(api=="wx.getBLEDeviceServices"){begin("services");[peripheral discoverServices:nil];return true;}
    if(api=="wx.getBLEDeviceRSSI"){begin("rssi");[peripheral readRSSI];return true;}
    auto service_id=ble_uuid(payload.at("serviceId").get<std::string>());CBService* service=nil;for(CBService* item in peripheral.services)if([item.UUID isEqual:service_id]){service=item;break;}if(!service)throw std::invalid_argument("Bluetooth service has not been discovered");
    if(api=="wx.getBLEDeviceCharacteristics"){begin("characteristics",service);[peripheral discoverCharacteristics:nil forService:service];return true;}
    auto characteristic_id=ble_uuid(payload.at("characteristicId").get<std::string>());CBCharacteristic* characteristic=nil;for(CBCharacteristic* item in service.characteristics)if([item.UUID isEqual:characteristic_id]){characteristic=item;break;}if(!characteristic)throw std::invalid_argument("Bluetooth characteristic has not been discovered");
    if(api=="wx.readBLECharacteristicValue"){begin("read",service,characteristic);[peripheral readValueForCharacteristic:characteristic];return true;}
    if(api=="wx.notifyBLECharacteristicValueChange"){begin("notify",service,characteristic);[peripheral setNotifyValue:payload.value("state",false) forCharacteristic:characteristic];return true;}
    if(api=="wx.writeBLECharacteristicValue"){auto bytes=binary_bytes(payload.at("value"));auto type=payload.value("writeType","")=="writeNoResponse"?CBCharacteristicWriteWithoutResponse:CBCharacteristicWriteWithResponse;if(bytes.size()>[peripheral maximumWriteValueLengthForType:type])throw std::invalid_argument("Bluetooth write exceeds negotiated MTU");if(type==CBCharacteristicWriteWithoutResponse&&!peripheral.canSendWriteWithoutResponse)throw std::runtime_error("Bluetooth write queue is full");NSData* data=[NSData dataWithBytes:bytes.data() length:bytes.size()];if(type==CBCharacteristicWriteWithResponse)begin("write",service,characteristic);[peripheral writeValue:data forCharacteristic:characteristic type:type];if(type==CBCharacteristicWriteWithoutResponse)done(call,api);return true;}
    return false;
  }
  std::set<std::string> recorder_managers_;
  void finish_recorder(uint64_t generation,bool success,NSError* error) {
    if(generation!=recorder_generation_||!recorder_)return;auto id=recorder_object_,path=recorder_path_;double duration=recorder_duration_;
    recorder_delegate_->finished={};recorder_.delegate=nil;recorder_=nil;recorder_delegate_=nil;recorder_object_.clear();recorder_path_.clear();
    if(!success){std::error_code ignored;std::filesystem::remove(recorder_temporary_,ignored);recorder_temporary_.clear();queue_event(weak_queue(),"Error",id,{{"errMsg",error?utf(error.localizedDescription):"system recording failed"},{"errCode",error.code}});return;}
    try{NSError* audio_error=nil;AVAudioFile* recorded=[[AVAudioFile alloc]initForReading:[NSURL fileURLWithPath:ns(recorder_temporary_.string())] error:&audio_error];if(recorded&&recorded.processingFormat.sampleRate>0)duration=double(recorded.length)/recorded.processingFormat.sampleRate;auto bytes=read_managed(recorder_temporary_,32*1024*1024);managed_write(root_,managed_path(root_,path),bytes);std::filesystem::remove(recorder_temporary_);recorder_temporary_.clear();queue_event(weak_queue(),"Stop",id,{{"tempFilePath",path},{"duration",duration*1000},{"fileSize",bytes.size()}});}
    catch(const std::exception& failure){std::error_code ignored;std::filesystem::remove(recorder_temporary_,ignored);recorder_temporary_.clear();queue_event(weak_queue(),"Error",id,{{"errMsg",failure.what()}});}
  }
  void stop_recorder(bool emit) {
    if(!recorder_)return;auto generation=recorder_generation_;recorder_duration_=recorder_.currentTime;[recorder_ stop];if(emit)finish_recorder(generation,true,nil);else{recorder_delegate_->finished={};recorder_.delegate=nil;recorder_=nil;recorder_delegate_=nil;recorder_object_.clear();recorder_path_.clear();++recorder_generation_;std::error_code ignored;std::filesystem::remove(recorder_temporary_,ignored);recorder_temporary_.clear();}
  }
  void start_recorder(const std::string& call,const std::string& api,const Json& payload) {
    if(recorder_)throw std::invalid_argument("a recording already owns the microphone");if(!foreground_)throw std::invalid_argument("recording requires a foreground application");
    auto id=payload.at("objectId").get<std::string>();if(!recorder_managers_.count(id))throw std::invalid_argument("recording manager was retired");
    auto format=payload.value("format","mp3");if(format!="aac"&&format!="wav")throw std::invalid_argument("macOS native recording supports explicit aac or wav format");
    if(payload.value("frameSize",0)>0)throw std::invalid_argument("encoded recording frame delivery is not supported by AVAudioRecorder");
    int duration=payload.value("duration",60000),rate=payload.value("sampleRate",44100),channels=payload.value("numberOfChannels",1),bitrate=payload.value("encodeBitRate",128000);
    if(duration<1||duration>600000||(rate!=8000&&rate!=16000&&rate!=22050&&rate!=44100&&rate!=48000)||channels<1||channels>2||bitrate<16000||bitrate>320000)throw std::invalid_argument("invalid native recording format or duration");
    if(format=="wav"&&int64_t(duration)*rate*channels*2/1000>32*1024*1024)throw std::invalid_argument("requested PCM recording exceeds managed file budget");
    recorder_path_="wxfile://user/recording-"+utf(NSUUID.UUID.UUIDString)+(format=="wav"?".wav":".m4a");recorder_object_=id;recorder_duration_=0;auto generation=++recorder_generation_;auto weak=weak_queue();
    NSDictionary* settings=format=="wav"?@{AVFormatIDKey:@(kAudioFormatLinearPCM),AVSampleRateKey:@(rate),AVNumberOfChannelsKey:@(channels),AVLinearPCMBitDepthKey:@16,AVLinearPCMIsFloatKey:@NO,AVLinearPCMIsBigEndianKey:@NO}:@{AVFormatIDKey:@(kAudioFormatMPEG4AAC),AVSampleRateKey:@(rate),AVNumberOfChannelsKey:@(channels),AVEncoderBitRateKey:@(bitrate)};
    recorder_temporary_=std::filesystem::u8path(options_.auxiliary_root)/(".weapp-recording-"+utf(NSUUID.UUID.UUIDString)+(format=="wav"?".wav":".m4a"));
    NSError* error=nil;recorder_=[[AVAudioRecorder alloc]initWithURL:[NSURL fileURLWithPath:ns(recorder_temporary_.string())] settings:settings error:&error];if(error||!recorder_){recorder_=nil;std::error_code ignored;std::filesystem::remove(recorder_temporary_,ignored);recorder_temporary_.clear();throw std::runtime_error(error?utf(error.localizedDescription):"cannot create native recorder");}
    recorder_delegate_=[WCMacRecorder new];recorder_delegate_->finished=[this,weak,generation](bool success,NSError* error){queue_action(weak,[this,weak,generation,success,error]{auto queue=weak.lock();if(queue&&!queue->closed){if(recorder_)recorder_duration_=recorder_.currentTime;finish_recorder(generation,success,error);}});};recorder_.delegate=recorder_delegate_;
    if(![recorder_ prepareToRecord]||![recorder_ recordForDuration:double(duration)/1000]){stop_recorder(false);throw std::runtime_error("system microphone recording failed to start");}queue_event(weak,"Start",id,Json::object());done(call,api);
  }
  bool recorder_call(const std::string& call,const std::string& api,const Json& payload) {
    if(api=="wx.getRecorderManager"){auto id=payload.at("objectId").get<std::string>();recorder_managers_.insert(id);retain_object(id,call,"RecorderManager",[this,id]{recorder_managers_.erase(id);if(recorder_object_==id)stop_recorder(false);});done(call,api,{{"objectId",id}});return true;}
    if(api.rfind("object:RecorderManager.",0)!=0)return false;require_object(payload,"RecorderManager");
    if(api=="object:RecorderManager.start"){
      if(![NSBundle.mainBundle objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"])throw std::invalid_argument("host Info.plist must declare NSMicrophoneUsageDescription");
      permit("scope.record",api,call,[this,call,api,payload]{auto weak=weak_queue();const auto generation=permission_generation_;auto start=[this,weak,call,api,payload,generation](bool granted){queue_action(weak,[this,weak,call,api,payload,generation,granted]{auto queue=weak.lock();if(!queue||queue->closed||!permission_call_is_current(call,generation))return;if(!granted){fail(call,api,"OS microphone permission denied","permission_denied");return;}try{start_recorder(call,api,payload);}catch(const std::exception& error){fail(call,api,error.what());}});};auto status=[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];if(status==AVAuthorizationStatusNotDetermined)[AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted){start(granted);}];else start(status==AVAuthorizationStatusAuthorized);});return true;
    }
    if(!recorder_||recorder_object_!=payload.value("objectId",""))throw std::invalid_argument("this recording manager has no active recording");
    if(api=="object:RecorderManager.pause"){[recorder_ pause];queue_event(weak_queue(),"Pause",recorder_object_,Json::object());}
    else if(api=="object:RecorderManager.resume"){if(!foreground_||![recorder_ record])throw std::runtime_error("recording could not resume");queue_event(weak_queue(),"Resume",recorder_object_,Json::object());}
    else if(api=="object:RecorderManager.stop")stop_recorder(true);else return false;done(call,api);return true;
  }
  void save_contact(const std::string& call,const std::string& api,const Json& payload) {
    CNMutableContact* contact=[CNMutableContact new];contact.givenName=ns(payload.value("firstName",""));contact.middleName=ns(payload.value("middleName",""));contact.familyName=ns(payload.value("lastName",""));contact.nickname=ns(payload.value("nickName",""));contact.organizationName=ns(payload.value("organization",""));contact.jobTitle=ns(payload.value("title",""));
    NSMutableArray* phones=[NSMutableArray array];for(auto item:{std::pair<const char*,NSString*>{"mobilePhoneNumber",CNLabelPhoneNumberMobile},{"workPhoneNumber",CNLabelWork},{"homePhoneNumber",CNLabelHome},{"hostNumber",CNLabelPhoneNumberMain}}){auto value=payload.value(item.first,"");if(!value.empty())[phones addObject:[CNLabeledValue labeledValueWithLabel:item.second value:[CNPhoneNumber phoneNumberWithStringValue:ns(value)]]];}contact.phoneNumbers=phones;
    auto email=payload.value("email","");if(!email.empty())contact.emailAddresses=@[[CNLabeledValue labeledValueWithLabel:CNLabelWork value:ns(email)]];
    if(contact.givenName.length+contact.familyName.length+contact.organizationName.length==0)throw std::invalid_argument("contact name or organization is required");CNSaveRequest* request=[CNSaveRequest new];[request addContact:contact toContainerWithIdentifier:nil];NSError* error=nil;if(![contacts_ executeSaveRequest:request error:&error]){fail(call,api,utf(error.localizedDescription),"invalid_request",error.code);return;}done(call,api);
  }
  void save_calendar(const std::string& call,const std::string& api,const Json& payload) {
    auto title=payload.value("title","");double start=payload.at("startTime").get<double>(),end=payload.value("endTime",start+3600);if(title.empty()||!std::isfinite(start)||!std::isfinite(end)||start<0||end<=start||end-start>366*86400)throw std::invalid_argument("invalid calendar title or time range");
    EKCalendar* target=calendar_.defaultCalendarForNewEvents;if(!target)throw std::runtime_error("system has no writable default calendar");EKEvent* event=[EKEvent eventWithEventStore:calendar_];event.calendar=target;event.title=ns(title);event.notes=ns(payload.value("description",""));event.location=ns(payload.value("location",""));event.startDate=[NSDate dateWithTimeIntervalSince1970:start];event.endDate=[NSDate dateWithTimeIntervalSince1970:end];event.allDay=payload.value("allDay",false);if(payload.value("alarm",false)){double offset=payload.value("alarmOffset",0.0);if(!std::isfinite(offset)||offset<0||offset>525600)throw std::invalid_argument("invalid calendar alarm offset");[event addAlarm:[EKAlarm alarmWithRelativeOffset:-offset*60]];}
    NSError* error=nil;if(![calendar_ saveEvent:event span:EKSpanThisEvent commit:YES error:&error]){fail(call,api,utf(error.localizedDescription),"invalid_request",error.code);return;}done(call,api);
  }
  static std::string authorization_state(NSInteger status,NSInteger granted,NSInteger denied,NSInteger restricted){return status==granted?"authorized":status==denied||status==restricted?"denied":"not determined";}
  Json app_authorization() const {
    auto camera=[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo],microphone=[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];auto location=location_?location_.authorizationStatus:kCLAuthorizationStatusNotDetermined;
    return {{"cameraAuthorized",authorization_state(camera,AVAuthorizationStatusAuthorized,AVAuthorizationStatusDenied,AVAuthorizationStatusRestricted)},{"microphoneAuthorized",authorization_state(microphone,AVAuthorizationStatusAuthorized,AVAuthorizationStatusDenied,AVAuthorizationStatusRestricted)},{"locationAuthorized",location==kCLAuthorizationStatusAuthorizedAlways?"authorized":location==kCLAuthorizationStatusDenied||location==kCLAuthorizationStatusRestricted?"denied":"not determined"},{"bluetoothAuthorized",authorization_state(CBManager.authorization,CBManagerAuthorizationAllowedAlways,CBManagerAuthorizationDenied,CBManagerAuthorizationRestricted)}};
  }
  bool system_call(const std::string& call,const std::string& api,const Json& payload) {
    if(api=="wx.openAppAuthorizeSetting"){if(![NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy"]])throw std::runtime_error("cannot open system privacy settings");done(call,api);return true;}
    if(api=="wx.checkIsSupportSoterAuthentication"||api=="wx.checkIsSoterEnrolledInDevice"){LAContext* context=[LAContext new];NSError* error=nil;BOOL enrolled=[context canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics error:&error];bool available=enrolled||(error.code==LAErrorBiometryNotEnrolled||error.code==LAErrorBiometryLockout);auto mode=context.biometryType==LABiometryTypeFaceID?"facial":"fingerPrint";
      if(api=="wx.checkIsSupportSoterAuthentication")done(call,api,{{"supportMode",available?Json::array({mode}):Json::array()}});else{auto requested=payload.value("checkAuthMode","");if(requested!="fingerPrint"&&requested!="facial")throw std::invalid_argument("unknown biometric authentication mode");done(call,api,{{"isEnrolled",bool((enrolled||error.code==LAErrorBiometryLockout)&&requested==mode)}});}[context invalidate];return true;}
    if(api=="wx.addPhoneContact"||api=="wx.addPhoneCalendar"){
      auto contacts=api=="wx.addPhoneContact";auto description=contacts?@"NSContactsUsageDescription":@"NSCalendarsUsageDescription";
      if(![NSBundle.mainBundle objectForInfoDictionaryKey:description]&&!( !contacts&&[NSBundle.mainBundle objectForInfoDictionaryKey:@"NSCalendarsWriteOnlyAccessUsageDescription"]))throw std::invalid_argument("host Info.plist must declare contact/calendar permission purpose");
      permit(contacts?"scope.addPhoneContact":"scope.addPhoneCalendar",api,call,[this,call,api,payload,contacts]{auto weak=weak_queue();const auto generation=permission_generation_;auto completed=^(BOOL granted,NSError* error){queue_action(weak,[this,weak,call,api,payload,contacts,generation,granted,error]{auto queue=weak.lock();if(!queue||queue->closed||!permission_call_is_current(call,generation))return;if(!granted){fail(call,api,error?utf(error.localizedDescription):"OS permission denied","permission_denied",error.code);return;}try{if(contacts)save_contact(call,api,payload);else save_calendar(call,api,payload);}catch(const std::exception& error){fail(call,api,error.what());}});};if(contacts)[contacts_ requestAccessForEntityType:CNEntityTypeContacts completionHandler:completed];else if(@available(macOS 14.0,*))[calendar_ requestWriteOnlyAccessToEventsWithCompletion:completed];else [calendar_ requestAccessToEntityType:EKEntityTypeEvent completion:completed];});return true;
    }
    return false;
  }
  NSURL* source(const std::string& value) const {
    if(value.rfind("wxfile://user/",0)==0)return [NSURL fileURLWithPath:ns(managed_path(root_,value).string())];
    NSURL* url=[NSURL URLWithString:ns(value)];if(!url||!allowed_origin(origin(url)))throw std::invalid_argument("URL origin is not allowed by trusted host network policy");return url;
  }
  void receive(NSURLSessionWebSocketTask* task,const std::string& object) {
    auto weak=weak_queue();[task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* message,NSError* error){
      if(error){queue_event(weak,"Error",object,{{"errMsg",utf(error.localizedDescription)},{"errCode",error.code}});return;}
      Json data;if(message.type==NSURLSessionWebSocketMessageTypeString)data=utf(message.string);
      else {auto begin=(const uint8_t*)message.data.bytes;if(message.data.length>1048576){[task cancelWithCloseCode:NSURLSessionWebSocketCloseCodeMessageTooBig reason:nil];queue_event(weak,"Error",object,{{"errMsg","WebSocket message exceeds one MiB limit"}});return;}data=inline_binary(std::vector<uint8_t>(begin,begin+message.data.length));}
      queue_event(weak,"Message",object,{{"data",data}});
      queue_action(weak,[this,weak,task,object]{auto queue=weak.lock();if(queue&&!queue->closed&&tasks_.count(object))receive(task,object);});
    }];
  }
  void network(const std::string& call,const std::string& api,const Json& payload) {
    NSURL* url=source(payload.value("url",""));auto object=payload.value("objectId","");
    NSMutableURLRequest* request=[NSMutableURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalAndRemoteCacheData timeoutInterval:std::clamp(payload.value("timeout",60000.0)/1000.0,0.1,300.0)];
    request.HTTPMethod=ns(payload.value("method",api=="wx.uploadFile"?"POST":"GET"));
    auto headers=payload.value("header",Json::object());if(!headers.is_object())throw std::invalid_argument("request headers must be an object");
    for(auto item=headers.begin();item!=headers.end();++item){if(!item.value().is_string()||item.key().find_first_of("\r\n:")!=std::string::npos||item.value().get<std::string>().find_first_of("\r\n")!=std::string::npos)throw std::invalid_argument("invalid request header");[request setValue:ns(item.value().get<std::string>()) forHTTPHeaderField:ns(item.key())];}
    if(api=="wx.connectSocket") {
      if(![url.scheme.lowercaseString isEqualToString:@"wss"])throw std::invalid_argument("WebSocket requires WSS");
      auto protocols=payload.value("protocols",Json::array());NSMutableArray<NSString*>* values=[NSMutableArray array];for(const auto& item:protocols)[values addObject:ns(item.get<std::string>())];
      if(values.count)[request setValue:[values componentsJoinedByString:@", "] forHTTPHeaderField:@"Sec-WebSocket-Protocol"];
      auto task=[session_ webSocketTaskWithRequest:request];task.maximumMessageSize=1048576;tasks_[object]=task;
      {std::lock_guard<std::mutex> lock(network_->state->mutex);network_->socketObjects[task.taskIdentifier]=object;}
      retain_object(object,call,"SocketTask",[this,object,task]{[task cancelWithCloseCode:NSURLSessionWebSocketCloseCodeGoingAway reason:nil];tasks_.erase(object);});
      [task resume];done(call,api,{{"objectId",object}});receive(task,object);return;
    }
    if(api=="wx.uploadFile") {
      auto bytes=managed_read(root_,managed_path(root_,payload.value("filePath","")),32*1024*1024);auto boundary="weapp-"+utf(NSUUID.UUID.UUIDString);auto name=payload.value("name","");
      if(name.empty()||name.find_first_of("\r\n\"")!=std::string::npos)throw std::invalid_argument("invalid upload field name");
      std::string prefix;
      auto form=payload.value("formData",Json::object());for(auto field=form.begin();field!=form.end();++field){if(field.key().find_first_of("\r\n\"")!=std::string::npos)throw std::invalid_argument("invalid multipart field name");prefix+="--"+boundary+"\r\nContent-Disposition: form-data; name=\""+field.key()+"\"\r\n\r\n"+(field.value().is_string()?field.value().get<std::string>():field.value().dump())+"\r\n";}
      prefix+="--"+boundary+"\r\nContent-Disposition: form-data; name=\""+name+"\"; filename=\"upload\"\r\nContent-Type: application/octet-stream\r\n\r\n";auto suffix="\r\n--"+boundary+"--\r\n";
      if(prefix.size()+bytes.size()+suffix.size()>32*1024*1024)throw std::invalid_argument("multipart upload exceeds byte budget");NSMutableData* body=[NSMutableData dataWithBytes:prefix.data() length:prefix.size()];[body appendBytes:bytes.data() length:bytes.size()];[body appendBytes:suffix.data() length:suffix.size()];request.HTTPBody=body;[request setValue:ns("multipart/form-data; boundary="+boundary) forHTTPHeaderField:@"Content-Type"];
    }else if(payload.contains("data")) {std::vector<uint8_t> bytes;auto data=payload.at("data");if(data.is_object()&&data.contains("__weappBinary"))bytes=binary_bytes(data);else {auto value=data.is_string()?data.get<std::string>():data.dump();bytes.assign(value.begin(),value.end());}if(bytes.size()>1048576)throw std::invalid_argument("request body exceeds one MiB limit");request.HTTPBody=[NSData dataWithBytes:bytes.data() length:bytes.size()];}
    auto transfer=std::make_shared<Transfer>();transfer->queue=weak_queue();transfer->call=call;transfer->api=api;transfer->object=object;transfer->payload=payload;transfer->root=root_;
    NSURLSessionDataTask* task=[session_ dataTaskWithRequest:request];tasks_[object]=task;network_cancellations_[object]=transfer->cancelled;{std::lock_guard<std::mutex> lock(network_->state->mutex);network_->state->transfers[task.taskIdentifier]=transfer;}
    auto type=api=="wx.request"?"RequestTask":api=="wx.downloadFile"?"DownloadTask":"UploadTask";
    retain_object(object,call,type,[this,task,object,cancelled=transfer->cancelled]{cancel_device_work(weak_queue(),*cancelled);[task cancel];tasks_.erase(object);network_cancellations_.erase(object);});[task resume];
  }
  void component_event(Mount& mount,const std::string& event,Json value) {
    auto data=Json{{"sessionId",mount.snapshot.at("sessionId")},{"accountEpoch",mount.snapshot.at("accountEpoch")},{"nodeId",mount.snapshot.at("nodeId")},{"event",event},{"sequence",std::to_string(++mount.sequence)},{"value",std::move(value)}};
    auto status=command_("hostComponentEvent",std::get<Value::Object>(decode(data).data),nullptr);if(!status)throw std::runtime_error(status.message);
  }
  void destroy_mount(Mount& mount) {[mount.output setSampleBufferDelegate:nil queue:nullptr];[mount.capture stopRunning];[mount.player pause];mount.capture=nil;mount.player=nil;mount.photo=nil;mount.output=nil;mount.capture_delegate=nil;mount.video_output=nil;}
  NSURL* media_source(const std::string& value) const {
    if(value.rfind("wxfile://user/",0)!=0)throw std::invalid_argument("native media requires a managed file; download through the trusted network API first");
    return [NSURL fileURLWithPath:ns(managed_path(root_,value).string())];
  }
  void start_camera(const std::string& id,uint64_t incarnation) {
    auto found=mounts_.find(id);if(found==mounts_.end()||found->second.incarnation!=incarnation||!foreground_||!options_.native_parent)return;auto& current=found->second;current.permission_pending=false;
    NSError* error=nil;AVCaptureDevice* device=[AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];auto input=device?[AVCaptureDeviceInput deviceInputWithDevice:device error:&error]:nil;
    if(error||!input){component_event(current,"error",{{"errMsg",error?utf(error.localizedDescription):"no camera available"}});return;}
    current.capture=[AVCaptureSession new];current.capture.sessionPreset=AVCaptureSessionPreset640x480;current.photo=[AVCapturePhotoOutput new];current.output=[AVCaptureVideoDataOutput new];current.output.videoSettings=@{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA)};current.output.alwaysDiscardsLateVideoFrames=YES;
    if(![current.capture canAddInput:input]||![current.capture canAddOutput:current.photo]||![current.capture canAddOutput:current.output]){component_event(current,"error",{{"errMsg","camera capture configuration unavailable"}});destroy_mount(current);return;}
    [current.capture addInput:input];[current.capture addOutput:current.photo];[current.capture addOutput:current.output];current.capture_delegate=[WCDeviceFrames new];current.capture_delegate->queue=weak_queue();current.capture_delegate->mailbox=media_;current.capture_delegate->node=id;current.capture_delegate->incarnation=incarnation;current.capture_delegate->context=media_context_;
    [current.output setSampleBufferDelegate:current.capture_delegate queue:dispatch_queue_create("weapp.camera.frames",DISPATCH_QUEUE_SERIAL)];[current.capture startRunning];component_event(current,"initdone",{{"width",640},{"height",480}});
  }
  void authorize_camera(const std::string& id) {
    auto& mount=mounts_.at(id);auto consent=consents_.find("scope.camera");if(mount.permission_denied){auto os=[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];if(consent==consents_.end()||!consent->second||os==AVAuthorizationStatusDenied||os==AVAuthorizationStatusRestricted)return;mount.permission_denied=false;}if(mount.permission_pending||mount.capture)return;mount.permission_pending=true;auto incarnation=mount.incarnation;auto weak=weak_queue();
    auto os=[this,weak,id,incarnation]{auto queue=weak.lock();if(!queue||queue->closed||!mounts_.count(id)||mounts_.at(id).incarnation!=incarnation)return;
      if(![NSBundle.mainBundle objectForInfoDictionaryKey:@"NSCameraUsageDescription"]){mounts_.at(id).permission_pending=false;mounts_.at(id).permission_denied=true;component_event(mounts_.at(id),"error",{{"errMsg","host Info.plist must declare NSCameraUsageDescription"}});return;}
      auto status=[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
      if(status==AVAuthorizationStatusAuthorized)start_camera(id,incarnation);
      else if(status==AVAuthorizationStatusNotDetermined)[AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted){queue_action(weak,[this,weak,id,incarnation,granted]{auto queue=weak.lock();if(!queue||queue->closed||!mounts_.count(id)||mounts_.at(id).incarnation!=incarnation)return;mounts_.at(id).permission_pending=false;if(granted)start_camera(id,incarnation);else {mounts_.at(id).permission_denied=true;component_event(mounts_.at(id),"error",{{"errMsg","OS camera permission denied"}});}});}];
      else {mounts_.at(id).permission_pending=false;mounts_.at(id).permission_denied=true;component_event(mounts_.at(id),"error",{{"errMsg","OS camera permission denied"}});}
    };
    if(consent!=consents_.end()){if(consent->second)os();else{mount.permission_pending=false;mount.permission_denied=true;component_event(mount,"error",{{"errMsg","application camera permission was denied; reset it in application settings"}});}return;}
    request_permission("scope.camera",[this,weak,id,incarnation,os](bool granted){queue_action(weak,[this,weak,id,incarnation,os,granted]{auto queue=weak.lock();if(!queue||queue->closed||!mounts_.count(id)||mounts_.at(id).incarnation!=incarnation)return;consents_["scope.camera"]=granted;auto text=Json(consents_).dump();durable_write(consents_file_,std::vector<uint8_t>(text.begin(),text.end()));if(granted)os();else {mounts_.at(id).permission_pending=false;mounts_.at(id).permission_denied=true;component_event(mounts_.at(id),"error",{{"errMsg","application camera permission denied"}});}});});
  }
  Status foreground_changed(bool visible) override {
    for(auto& entry:mounts_){auto& mount=entry.second;if(!visible){[mount.capture stopRunning];[mount.player pause];}else if(mount.capture && mount.snapshot.value("visible",false))[mount.capture startRunning];}
    if(!visible){close_bluetooth();if(recorder_.recording){[recorder_ pause];queue_event(weak_queue(),"InterruptionBegin",recorder_object_,Json::object());}cancel_locations("location request was retired when the application entered background");for(auto& entry:audio_)[entry.second.player pause];}return {};
  }
  void request_permission(const std::string& scope,std::function<void(bool)> completion) override {
    if(scope=="open-settings") {
      NSAlert* alert=[NSAlert new];alert.messageText=@"Application permissions";alert.informativeText=ns("Manage permissions for "+encode(options_.application).value("appId","")+". Choosing Reset revokes this application's saved permissions.");[alert addButtonWithTitle:@"Done"];[alert addButtonWithTitle:@"Reset permissions"];
      auto weak=weak_queue();auto show=^(NSModalResponse response){queue_action(weak,[this,weak,response,completion]{auto queue=weak.lock();if(!queue||queue->closed)return;if(response==NSAlertSecondButtonReturn){reset_permissions();}completion(true);});};
      NSWindow* parent=options_.native_parent?[(__bridge NSView*)options_.native_parent window]:nil;if(parent){dialogs_.push_back(alert.window);[alert beginSheetModalForWindow:parent completionHandler:show];}else{completion(false);}return;
    }
    NSWindow* parent=options_.native_parent?[(__bridge NSView*)options_.native_parent window]:nil;
    if(!parent){completion(false);return;}
    NSAlert* alert=[NSAlert new];alert.messageText=@"Allow application access?";alert.informativeText=ns(encode(options_.application).value("appId","")+" requests "+scope);[alert addButtonWithTitle:@"Allow"];[alert addButtonWithTitle:@"Deny"];dialogs_.push_back(alert.window);
    [alert beginSheetModalForWindow:parent completionHandler:^(NSModalResponse response){completion(response==NSAlertFirstButtonReturn);}];
  }
  void detach_location_manager() {
    ++location_generation_;
    [location_ stopUpdatingLocation];location_.delegate=nil;
    if(location_delegate_){location_delegate_->update={};location_delegate_->authorization={};}
    location_=nil;location_delegate_=nil;
  }
  void cancel_locations(const std::string& message,const std::string& code="cancelled") {
    for(const auto& [call,intent]:location_intents_){*intent.delivery=false;if(calls_.count(call))fail(call,calls_.at(call).context.value("api","wx.getLocation"),message,code);}
    if(location_watch_delivery_)*location_watch_delivery_=false;
    if(location_terminal_delivery_)*location_terminal_delivery_=false;
    continuous_location_=false;location_started_=false;location_calls_.clear();detach_location_manager();
  }
  void location_backpressure() {
    bool watched=continuous_location_;
    cancel_locations("native location ingress backpressure limit exceeded");
    if(watched){
      location_terminal_delivery_=std::make_shared<std::atomic<bool>>(true);
      DeviceQueued event{DeviceQueued::NativeTerminalEvent,"","LocationChangeError","",{{"errMsg","native location ingress backpressure limit exceeded"}}};
      event.delivery_allowed=location_terminal_delivery_;queue_->push(std::move(event));
    }
  }
  void location_receipt(const std::string& call,const std::string& api,Json value,const std::shared_ptr<std::atomic<bool>>& delivery) {
    DeviceQueued item{DeviceQueued::Complete,call,"","",device_ok(api,std::move(value))};item.delivery_allowed=delivery;
    if(!queue_->push(std::move(item)))fail(call,api,"native location completion queue is full");
  }
  void ensure_location_manager() {
    if(location_)return;
    location_delegate_=[WCDeviceLocation new];location_=[CLLocationManager new];location_.delegate=location_delegate_;
    auto weak=weak_queue();auto generation=++location_generation_;
    if(location_terminal_delivery_)*location_terminal_delivery_=false;
    location_overflow_=std::make_shared<std::atomic<bool>>(false);auto overflow=location_overflow_;
    location_delegate_->update=[this,weak,generation,overflow](CLLocation* value,NSError* error){
      if(!queue_action(weak,[this,weak,generation,value,error]{
        auto queue=weak.lock();if(!queue||queue->closed||generation!=location_generation_||!foreground_)return;
        Json result;std::string failure;int64_t native=0;
        try {
          if(error){failure=utf(error.localizedDescription);native=error.code;}
          else if(value){
            result={{"latitude",value.coordinate.latitude},{"longitude",value.coordinate.longitude},{"accuracy",value.horizontalAccuracy},{"horizontalAccuracy",value.horizontalAccuracy}};
            if(value.verticalAccuracy>=0){result["altitude"]=value.altitude;result["verticalAccuracy"]=value.verticalAccuracy;}
            if(value.speed>=0)result["speed"]=value.speed;
            result=location_position(std::move(result),false);
          }else failure="OS location returned no position";
        }catch(const std::exception& invalid){failure=invalid.what();}
        auto pending=std::move(location_calls_);location_calls_.clear();
        if(failure.empty()&&continuous_location_)location_started_=true;
        for(const auto& [call,request]:pending)if(calls_.count(call)){
          auto api=calls_.at(call).context.value("api","wx.getLocation");
          if(!failure.empty())fail(call,api,failure,"invalid_request",native);
          else location_receipt(call,api,request.options.continuous?Json::object():location_position(result,request.options.fuzzy),request.delivery);
        }
        if(continuous_location_){
          DeviceQueued event{DeviceQueued::NativeEvent,"",failure.empty()?"LocationChange":"LocationChangeError","",failure.empty()?result:Json{{"errMsg",failure},{"errCode",native}}};
          event.delivery_allowed=location_watch_delivery_;if(!queue_->push(std::move(event)))location_backpressure();
        }else [location_ stopUpdatingLocation];
      }))*overflow=true;
    };
    location_delegate_->authorization=[this,weak,generation,overflow](CLAuthorizationStatus status){
      if(!queue_action(weak,[this,weak,generation,status]{
        auto queue=weak.lock();if(!queue||queue->closed||generation!=location_generation_||!foreground_)return;
        if(status==kCLAuthorizationStatusAuthorizedAlways){if(continuous_location_||!location_calls_.empty())[location_ startUpdatingLocation];}
        else if(status==kCLAuthorizationStatusDenied||status==kCLAuthorizationStatusRestricted)cancel_locations("OS location permission denied","permission_denied");
      }))*overflow=true;
    };
  }
  void refresh_location_accuracy() {
    bool precise=continuous_location_,high=continuous_location_&&location_watch_options_.high_accuracy;
    for(const auto& [call,request]:location_calls_){precise|=!request.options.fuzzy;high|=request.options.high_accuracy;}
    location_.desiredAccuracy=high?kCLLocationAccuracyBest:precise?kCLLocationAccuracyHundredMeters:kCLLocationAccuracyThreeKilometers;
  }
  void locate(const std::string& call,const std::string& api,const LocationOptions& options,const std::shared_ptr<std::atomic<bool>>& intent) {
    if(!intent->load()){fail(call,api,"location request was retired","cancelled");return;}
    if(!foreground_){fail(call,api,"location requires a foreground application","permission_denied");return;}
    if(![NSBundle.mainBundle objectForInfoDictionaryKey:@"NSLocationWhenInUseUsageDescription"]) {fail(call,api,"host Info.plist must declare NSLocationWhenInUseUsageDescription","permission_denied");return;}
    if(options.continuous&&continuous_location_){
      if(options.high_accuracy!=location_watch_options_.high_accuracy||options.timeout_ms!=location_watch_options_.timeout_ms)throw std::invalid_argument("stop the active location watch before changing its options");
      if(!location_started_)throw std::invalid_argument("location watch startup is already pending");
      location_receipt(call,api,Json::object(),intent);return;
    }
    if(location_calls_.size()>=8)throw std::invalid_argument("native location concurrency limit exceeded");
    ensure_location_manager();auto status=location_.authorizationStatus;
    if(status==kCLAuthorizationStatusDenied||status==kCLAuthorizationStatusRestricted){fail(call,api,"OS location permission denied","permission_denied");return;}
    location_calls_[call]={options,intent,std::chrono::steady_clock::now()+std::chrono::milliseconds(options.timeout_ms)};
    if(options.continuous){continuous_location_=true;location_started_=false;location_watch_options_=options;location_watch_delivery_=intent;}
    refresh_location_accuracy();
    calls_.at(call).cancel=[this,call,intent]{
      *intent=false;location_calls_.erase(call);
      if(location_watch_delivery_==intent){continuous_location_=false;location_started_=false;detach_location_manager();if(!location_calls_.empty()){ensure_location_manager();refresh_location_accuracy();[location_ startUpdatingLocation];}}
      else if(location_calls_.empty()&&!continuous_location_)[location_ stopUpdatingLocation];else refresh_location_accuracy();
    };
    if(status==kCLAuthorizationStatusNotDetermined)[location_ requestWhenInUseAuthorization];else [location_ startUpdatingLocation];
  }
  void stop_location(const std::string& call,const std::string& api) {
    for(const auto& [pending,intent]:location_intents_)if(intent.continuous){*intent.delivery=false;if(calls_.count(pending))fail(pending,"wx.startLocationUpdate","location update was stopped","cancelled");}
    if(location_watch_delivery_)*location_watch_delivery_=false;
    if(location_terminal_delivery_)*location_terminal_delivery_=false;
    continuous_location_=false;location_started_=false;
    for(auto item=location_calls_.begin();item!=location_calls_.end();)if(item->second.options.continuous)item=location_calls_.erase(item);else ++item;
    detach_location_manager();
    if(!location_calls_.empty()){ensure_location_manager();refresh_location_accuracy();[location_ startUpdatingLocation];}
    done(call,api);
  }
  bool platform_call(const std::string& call,const std::string& api,const Json& payload,const Json& context) override {
    if(bluetooth_call(call,api,payload)||recorder_call(call,api,payload)||system_call(call,api,payload))return true;
    if(api=="wx.request"||api=="wx.downloadFile"||api=="wx.uploadFile"||api=="wx.connectSocket"){network(call,api,payload);return true;}
    if(api.rfind("object:",0)==0 && api.find("Task.")!=std::string::npos){auto id=payload.value("objectId","");auto found=tasks_.find(id);if(found==tasks_.end())throw std::invalid_argument("native task is unavailable");auto task=found->second;
      if(api.find(".abort")!=std::string::npos){if(network_cancellations_.count(id))cancel_device_work(weak_queue(),*network_cancellations_.at(id));[task cancel];done(call,api);return true;}
      if(api=="object:SocketTask.close"){auto code=payload.value("code",1000);auto text=payload.value("reason","");[(NSURLSessionWebSocketTask*)task cancelWithCloseCode:(NSURLSessionWebSocketCloseCode)code reason:[ns(text)dataUsingEncoding:NSUTF8StringEncoding]];done(call,api);return true;}
      if(api=="object:SocketTask.send"){auto data=payload.at("data");NSURLSessionWebSocketMessage* message;if(data.is_string())message=[[NSURLSessionWebSocketMessage alloc]initWithString:ns(data.get<std::string>())];else{auto bytes=binary_bytes(data);message=[[NSURLSessionWebSocketMessage alloc]initWithData:[NSData dataWithBytes:bytes.data() length:bytes.size()]];}auto weak=weak_queue();[(NSURLSessionWebSocketTask*)task sendMessage:message completionHandler:^(NSError* error){if(error)queue_failure(weak,call,device_failure(api,utf(error.localizedDescription),"invalid_request",error.code));else queue_complete(weak,call,device_ok(api));}];return true;}}
    if(api=="wx.getClipboardData"){permit("scope.clipboard",api,call,[this,call,api]{done(call,api,{{"data",utf([NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString])}});});return true;}
    if(api=="wx.setClipboardData"){auto text=payload.at("data").get<std::string>();permit("scope.clipboard",api,call,[this,call,api,text]{auto pasteboard=NSPasteboard.generalPasteboard;[pasteboard clearContents];if(![pasteboard setString:ns(text) forType:NSPasteboardTypeString]){fail(call,api,"system clipboard write failed");return;}done(call,api);});return true;}
    if(api=="wx.showModal"||api=="wx.showActionSheet") {
      NSWindow* parent=options_.native_parent?[(__bridge NSView*)options_.native_parent window]:nil;if(!parent)throw std::invalid_argument("native dialog requires an attached host view");NSAlert* alert=[NSAlert new];alert.messageText=ns(payload.value("title",""));alert.informativeText=ns(payload.value("content",""));
      if(api=="wx.showModal"){[alert addButtonWithTitle:ns(payload.value("confirmText","OK"))];if(payload.value("showCancel",true))[alert addButtonWithTitle:ns(payload.value("cancelText","Cancel"))];}
      else{auto items=payload.at("itemList");if(!items.is_array()||items.empty()||items.size()>6)throw std::invalid_argument("action sheet requires one to six items");for(const auto& item:items)[alert addButtonWithTitle:ns(item.get<std::string>())];[alert addButtonWithTitle:@"Cancel"];}
      dialogs_.push_back(alert.window);auto weak=weak_queue();auto count=api=="wx.showActionSheet"?payload["itemList"].size():0;
      [alert beginSheetModalForWindow:parent completionHandler:^(NSModalResponse response){if(api=="wx.showModal")queue_complete(weak,call,device_ok(api,{{"confirm",response==NSAlertFirstButtonReturn},{"cancel",response!=NSAlertFirstButtonReturn}}));else if(response>=NSAlertFirstButtonReturn&&size_t(response-NSAlertFirstButtonReturn)<count)queue_complete(weak,call,device_ok(api,{{"tapIndex",response-NSAlertFirstButtonReturn}}));else queue_failure(weak,call,device_failure(api,"user cancelled"));}];return true;
    }
    if(api=="wx.getLocation"||api=="wx.getFuzzyLocation"||api=="wx.startLocationUpdate") {
      if(!foreground_)throw std::invalid_argument("location requires a foreground application");
      auto options=location_options(api,payload);auto intent=std::make_shared<std::atomic<bool>>(true);location_intents_[call]={intent,options.continuous};
      calls_.at(call).cancel=[intent]{*intent=false;};
      permit(options.fuzzy?"scope.userFuzzyLocation":"scope.userLocation",api,call,[this,call,api,options,intent]{try{locate(call,api,options,intent);}catch(const std::exception& error){fail(call,api,error.what());}});return true;
    }
    if(api=="wx.stopLocationUpdate"){stop_location(call,api);return true;}
    if(api=="wx.openLocation"){double latitude=payload.at("latitude").get<double>(),longitude=payload.at("longitude").get<double>();if(!std::isfinite(latitude)||!std::isfinite(longitude)||fabs(latitude)>90||fabs(longitude)>180)throw std::invalid_argument("invalid location coordinate");NSURL* url=[NSURL URLWithString:ns("https://maps.apple.com/?ll="+std::to_string(latitude)+","+std::to_string(longitude))];if(![NSWorkspace.sharedWorkspace openURL:url])throw std::runtime_error("cannot open system map application");done(call,api);return true;}
    if(api=="wx.getNetworkType"){SCNetworkReachabilityRef reachability=SCNetworkReachabilityCreateWithName(nullptr,"apple.com");SCNetworkReachabilityFlags flags=0;bool reachable=reachability&&SCNetworkReachabilityGetFlags(reachability,&flags)&&(flags&kSCNetworkReachabilityFlagsReachable);if(reachability)CFRelease(reachability);done(call,api,{{"networkType",reachable?([CWWiFiClient.sharedWiFiClient interface].powerOn?"wifi":"ethernet"):"none"}});return true;}
    if(api=="wx.getBatteryInfo"){done(call,api,battery());return true;}
    if(api=="wx.setKeepScreenOn"){if(payload.value("keepScreenOn",false)){if(wake_==kIOPMNullAssertionID&&IOPMAssertionCreateWithName(kIOPMAssertionTypeNoDisplaySleep,kIOPMAssertionLevelOn,CFSTR("Weapp active screen"),&wake_)!=kIOReturnSuccess)throw std::runtime_error("cannot acquire display wake assertion");}else if(wake_!=kIOPMNullAssertionID){IOPMAssertionRelease(wake_);wake_=kIOPMNullAssertionID;}done(call,api);return true;}
    if(api=="wx.startWifi"||api=="wx.stopWifi"){if(!CWWiFiClient.sharedWiFiClient.interface)throw std::runtime_error("system has no Wi-Fi interface");done(call,api);return true;}
    if(api=="wx.getConnectedWifi"){permit("scope.userLocation",api,call,[this,call,api]{auto wifi=CWWiFiClient.sharedWiFiClient.interface;if(!wifi||!wifi.ssid){fail(call,api,"Wi-Fi identity unavailable; OS location authorization may be required","permission_denied");return;}done(call,api,{{"wifi",{{"SSID",utf(wifi.ssid)},{"BSSID",utf(wifi.bssid)},{"signalStrength",wifi.rssiValue},{"secure",wifi.security!=kCWSecurityNone}}}});});return true;}
    if(api=="wx.getWifiList"){permit("scope.userLocation",api,call,[this,call,api]{NSError* error=nil;auto networks=[CWWiFiClient.sharedWiFiClient.interface scanForNetworksWithSSID:nil error:&error];if(error){fail(call,api,utf(error.localizedDescription),"invalid_request",error.code);return;}Json list=Json::array();for(CWNetwork* network in networks){if(list.size()>=256)break;list.push_back({{"SSID",utf(network.ssid)},{"BSSID",utf(network.bssid)},{"signalStrength",network.rssiValue},{"secure",![network supportsSecurity:kCWSecurityNone]}});}queue_event(weak_queue(),"GetWifiList","",{{"wifiList",list}});done(call,api);});return true;}
    if(api=="wx.connectWifi"){auto ssid=payload.at("SSID").get<std::string>(),password=payload.value("password","");permit("scope.userLocation",api,call,[this,call,api,ssid,password]{NSError* error=nil;auto interface=CWWiFiClient.sharedWiFiClient.interface;auto networks=[interface scanForNetworksWithSSID:[ns(ssid)dataUsingEncoding:NSUTF8StringEncoding] error:&error];CWNetwork* target=networks.anyObject;if(error||!target||![interface associateToNetwork:target password:ns(password) error:&error]){fail(call,api,error?utf(error.localizedDescription):"Wi-Fi network unavailable","invalid_request",error.code);return;}queue_event(weak_queue(),"WifiConnected","",{{"wifi",{{"SSID",ssid},{"BSSID",utf(target.bssid)}}}});done(call,api);});return true;}
    if(api=="wx.createInnerAudioContext"){auto id=payload.at("objectId").get<std::string>();Audio audio;audio.player=[AVPlayer new];audio.properties={{"src",""},{"volume",1},{"loop",false},{"autoplay",false},{"playbackRate",1},{"startTime",0},{"paused",true},{"currentTime",0},{"duration",0},{"buffered",0}};auto player=audio.player;__weak AVPlayer* weak_player=player;auto weak=weak_queue();
      audio.timer=[player addPeriodicTimeObserverForInterval:CMTimeMake(1,4) queue:dispatch_get_main_queue() usingBlock:^(CMTime time){AVPlayer* sampled=weak_player;if(!sampled)return;queue_action(weak,[this,weak,id,sampled,time]{auto queue=weak.lock();if(!queue||queue->closed||!audio_.count(id)||audio_.at(id).player!=sampled)return;auto& state=audio_.at(id);double position=CMTimeGetSeconds(time),duration=CMTimeGetSeconds(sampled.currentItem.duration);state.properties["currentTime"]=std::isfinite(position)?position:0;state.properties["duration"]=std::isfinite(duration)?duration:0;double buffered=0;for(NSValue* range in sampled.currentItem.loadedTimeRanges){auto interval=range.CMTimeRangeValue;double end=CMTimeGetSeconds(CMTimeAdd(interval.start,interval.duration));if(std::isfinite(end))buffered=std::max(buffered,end);}state.properties["buffered"]=buffered;state.properties["paused"]=sampled.rate==0;queue_event(weak,"TimeUpdate",id,{{"properties",state.properties}});});}];
      audio.ended=[NSNotificationCenter.defaultCenter addObserverForName:AVPlayerItemDidPlayToEndTimeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* note){if(note.object!=player.currentItem)return;queue_action(weak,[this,weak,id,player]{auto queue=weak.lock();if(!queue||queue->closed||!audio_.count(id)||audio_.at(id).player!=player)return;queue_event(weak,"Ended",id,Json::object());if(audio_.at(id).properties.value("loop",false)&&foreground_){[player seekToTime:kCMTimeZero];[player play];}});}];
      audio_[id]=audio;retain_object(id,call,"InnerAudioContext",[this,id]{dispose_audio(id);});done(call,api,{{"objectId",id},{"properties",audio.properties}});return true;}
    if(api.rfind("object:InnerAudioContext.",0)==0){require_object(payload,"InnerAudioContext");auto id=payload.at("objectId").get<std::string>();auto& audio=audio_.at(id);auto member=api.substr(api.find_last_of('.')+1);
      if(payload.value("action","")=="set"){auto value=payload.at("value");if(member=="src"){NSURL* url=media_source(value.get<std::string>());[audio.player replaceCurrentItemWithPlayerItem:[AVPlayerItem playerItemWithURL:url]];[audio.player seekToTime:CMTimeMakeWithSeconds(audio.properties.value("startTime",0.0),600)];if(audio.properties.value("autoplay",false)&&foreground_)[audio.player play];}else if(member=="volume"){double number=value.get<double>();if(!std::isfinite(number)||number<0||number>1)throw std::invalid_argument("volume must be between zero and one");audio.player.volume=number;}else if(member=="playbackRate"){double rate=value.get<double>();if(rate<=0||rate>4)throw std::invalid_argument("invalid playback rate");if(audio.player.rate)audio.player.rate=rate;}else if(member=="startTime"){double seconds=value.get<double>();if(!std::isfinite(seconds)||seconds<0)throw std::invalid_argument("audio start time must be nonnegative");}else if(member=="loop"||member=="autoplay"){if(!value.is_boolean())throw std::invalid_argument("audio boolean property requires true or false");}else throw std::invalid_argument("audio property is read-only or unavailable");audio.properties[member]=value;done(call,api,{{"value",value}});return true;}
      if(member=="play"){if(!foreground_)throw std::invalid_argument("audio playback requires a foreground application");[audio.player play];audio.player.rate=audio.properties.value("playbackRate",1.0);queue_event(weak_queue(),"Play",id,Json::object());}
      else if(member=="pause"){[audio.player pause];queue_event(weak_queue(),"Pause",id,Json::object());}
      else if(member=="stop"){[audio.player pause];[audio.player seekToTime:kCMTimeZero];queue_event(weak_queue(),"Stop",id,Json::object());}
      else if(member=="seek"){auto args=payload.value("args",Json::array());double seconds=payload.value("position",args.empty()?0.0:args[0].get<double>());if(!std::isfinite(seconds)||seconds<0)throw std::invalid_argument("invalid audio seek time");[audio.player seekToTime:CMTimeMakeWithSeconds(seconds,600)];queue_event(weak_queue(),"Seeked",id,Json::object());}
      else if(member=="destroy")dispose_audio(id);else return false;done(call,api);return true;
    }
    if(api=="wx.createCameraContext"||api=="wx.createVideoContext"){auto object=payload.at("objectId").get<std::string>();auto target=context.at("componentTarget");auto node=target.at("nodeId").get<std::string>();auto found=mounts_.find(node);auto tag=api=="wx.createCameraContext"?"camera":"video";if(found==mounts_.end()||found->second.snapshot.value("tag","")!=tag||found->second.snapshot.at("pageId")!=target.at("pageId")||found->second.snapshot.at("ownerScopeId")!=target.at("ownerScopeId"))throw std::invalid_argument("trusted native component target is no longer mounted");if(api=="wx.createCameraContext")camera_nodes_[object]=node;else video_nodes_[object]=node;retain_object(object,call,api=="wx.createCameraContext"?"CameraContext":"VideoContext",[this,object]{camera_nodes_.erase(object);video_nodes_.erase(object);});done(call,api,{{"objectId",object}});return true;}
    if(api=="object:CameraContext.takePhoto"){require_object(payload,"CameraContext");auto& mount=mounts_.at(camera_nodes_.at(payload.at("objectId").get<std::string>()));if(!mount.capture.running||!mount.photo)throw std::runtime_error("camera capture is not authorized or ready");auto delegate=[WCDevicePhoto new];delegate->queue=weak_queue();delegate->call=call;delegate->api=api;delegate->path="wxfile://user/photo-"+utf(NSUUID.UUID.UUIDString)+".jpg";delegate->root=root_;delegate->cancelled=std::make_shared<std::atomic<bool>>(false);delegate->finished=std::make_shared<std::atomic<bool>>(false);calls_.at(call).cancel=[weak=weak_queue(),cancelled=delegate->cancelled]{cancel_device_work(weak,*cancelled);};photos_.push_back(delegate);[mount.photo capturePhotoWithSettings:[AVCapturePhotoSettings photoSettings] delegate:delegate];return true;}
    if(api.rfind("object:VideoContext.",0)==0){require_object(payload,"VideoContext");auto& mount=mounts_.at(video_nodes_.at(payload.at("objectId").get<std::string>()));auto member=api.substr(api.find_last_of('.')+1);if(member=="play"){if(!foreground_)throw std::invalid_argument("video playback requires a foreground application");[mount.player play];}else if(member=="pause")[mount.player pause];else if(member=="stop"){[mount.player pause];[mount.player seekToTime:kCMTimeZero];}else if(member=="seek"){auto args=payload.value("args",Json::array());double seconds=payload.value("position",args.empty()?0.0:args[0].get<double>());if(!std::isfinite(seconds)||seconds<0)throw std::invalid_argument("invalid video seek time");[mount.player seekToTime:CMTimeMakeWithSeconds(seconds,600)];}else return false;done(call,api);return true;}
    return false;
  }
  Json battery() const {CFTypeRef info=IOPSCopyPowerSourcesInfo();CFArrayRef sources=info?IOPSCopyPowerSourcesList(info):nullptr;Json result=Json::object();if(sources)for(CFIndex i=0;i<CFArrayGetCount(sources);++i){auto detail=IOPSGetPowerSourceDescription(info,CFArrayGetValueAtIndex(sources,i));NSDictionary* data=(__bridge NSDictionary*)detail;NSNumber* capacity=data[@kIOPSCurrentCapacityKey],*maximum=data[@kIOPSMaxCapacityKey];if(capacity&&maximum.intValue>0){result["level"]=capacity.intValue*100/maximum.intValue;result["isCharging"]=[data[@kIOPSIsChargingKey] boolValue];break;}}if(sources)CFRelease(sources);if(info)CFRelease(info);if(result.empty())throw std::runtime_error("system battery information is unavailable");return result;}
  void dispose_audio(const std::string& id){auto found=audio_.find(id);if(found==audio_.end())return;[found->second.player pause];if(found->second.timer)[found->second.player removeTimeObserver:found->second.timer];if(found->second.ended)[NSNotificationCenter.defaultCenter removeObserver:found->second.ended];audio_.erase(found);}
  void permissions_reset() override {
    {std::lock_guard<std::mutex> barrier(queue_->side_effects);for(auto photo:photos_)if(photo->cancelled)*photo->cancelled=true;}
    close_bluetooth();stop_recorder(false);for(auto& entry:calls_)if(entry.second.context.value("api","")=="object:RecorderManager.start"||entry.second.context.value("api","")=="object:CameraContext.takePhoto")fail(entry.first,entry.second.context.value("api",""),"application recording permission was reset","permission_denied");cancel_locations("application location permission was reset","permission_denied");
    for(auto& entry:mounts_)if(entry.second.snapshot.value("tag","")=="camera"){destroy_mount(entry.second);entry.second.incarnation=++mount_sequence_;entry.second.permission_denied=true;entry.second.permission_pending=false;}
  }
  void retire_platform() override {
    close_bluetooth();stop_recorder(false);recorder_managers_.clear();for(LAContext* context:authentication_)[context invalidate];authentication_.clear();
    cancel_locations("native location owner was retired");location_intents_.clear();
    [session_ invalidateAndCancel];tasks_.clear();network_cancellations_.clear();while(!audio_.empty())dispose_audio(audio_.begin()->first);
    for(auto& entry:mounts_)destroy_mount(entry.second);mounts_.clear();{std::lock_guard<std::mutex> lock(media_->mutex);media_->targets.clear();media_->latest.clear();media_->sequences.clear();media_->incarnations.clear();}camera_nodes_.clear();video_nodes_.clear();photos_.clear();
    for(NSWindow* dialog:dialogs_){if(dialog.sheetParent)[dialog.sheetParent endSheet:dialog returnCode:NSModalResponseCancel];[dialog orderOut:nil];}dialogs_.clear();
    if(wake_!=kIOPMNullAssertionID){IOPMAssertionRelease(wake_);wake_=kIOPMNullAssertionID;}
  }
 public:
  MacDeviceApis(const DeviceApiOptions& options,Command command):DeviceBase(options,std::move(command)) {
    network_=[WCDeviceNetwork new];network_->state=std::make_shared<NetworkState>();network_->state->origins=options.allowed_network_origins;network_->queue=weak_queue();
    auto configuration=NSURLSessionConfiguration.ephemeralSessionConfiguration;configuration.HTTPShouldSetCookies=NO;configuration.HTTPCookieStorage=nil;configuration.URLCredentialStorage=nil;configuration.URLCache=nil;NSOperationQueue* operations=[NSOperationQueue new];operations.maxConcurrentOperationCount=1;session_=[NSURLSession sessionWithConfiguration:configuration delegate:network_ delegateQueue:operations];
    for(auto api:{"wx.request","wx.downloadFile","wx.uploadFile","wx.connectSocket","object:RequestTask.abort","object:DownloadTask.abort","object:UploadTask.abort","object:SocketTask.send","object:SocketTask.close","wx.getClipboardData","wx.setClipboardData","wx.showModal","wx.showActionSheet","wx.getLocation","wx.getFuzzyLocation","wx.startLocationUpdate","wx.stopLocationUpdate","wx.openLocation","wx.getNetworkType","wx.getBatteryInfo","wx.setKeepScreenOn","wx.startWifi","wx.stopWifi","wx.getConnectedWifi","wx.getWifiList","wx.connectWifi","wx.createInnerAudioContext","wx.createCameraContext","object:CameraContext.takePhoto","wx.createVideoContext","object:VideoContext.play","object:VideoContext.pause","object:VideoContext.stop","object:VideoContext.seek","component:camera","component:video"})add(api);
    for(auto type:{"RequestTask","DownloadTask","UploadTask"}){add_event("object:"+std::string(type)+".onHeadersReceived","object:"+std::string(type)+".offHeadersReceived","HeadersReceived");if(std::string(type)!="RequestTask")add_event("object:"+std::string(type)+".onProgressUpdate","object:"+std::string(type)+".offProgressUpdate","ProgressUpdate");}
    add_event("object:RequestTask.onChunkReceived","object:RequestTask.offChunkReceived","ChunkReceived");
    for(auto event:{"Open","Message","Error","Close"})apis_["object:SocketTask.on"+std::string(event)]=event;
    for(auto event:{"LocationChange","LocationChangeError","GetWifiList","WifiConnected"})add_event("wx.on"+std::string(event),"wx.off"+std::string(event),event);
    for(auto member:{"play","pause","stop","seek","destroy","src","volume","loop","autoplay","startTime","playbackRate","paused","currentTime","duration","buffered"})add("object:InnerAudioContext."+std::string(member));
    for(auto event:{"Play","Pause","Stop","Seeked","Ended","TimeUpdate"})add_event("object:InnerAudioContext.on"+std::string(event),"object:InnerAudioContext.off"+std::string(event),event);
    for(auto api:{"wx.openAppAuthorizeSetting","wx.addPhoneContact","wx.addPhoneCalendar","wx.checkIsSupportSoterAuthentication","wx.checkIsSoterEnrolledInDevice","wx.getRecorderManager","object:RecorderManager.start","object:RecorderManager.stop","object:RecorderManager.pause","object:RecorderManager.resume","wx.openBluetoothAdapter","wx.closeBluetoothAdapter","wx.getBluetoothAdapterState","wx.startBluetoothDevicesDiscovery","wx.stopBluetoothDevicesDiscovery","wx.getBluetoothDevices","wx.getConnectedBluetoothDevices","wx.createBLEConnection","wx.closeBLEConnection","wx.getBLEDeviceServices","wx.getBLEDeviceCharacteristics","wx.readBLECharacteristicValue","wx.writeBLECharacteristicValue","wx.notifyBLECharacteristicValueChange","wx.getBLEDeviceRSSI","wx.getBLEMTU","wx.openSystemBluetoothSetting"})add(api);
    for(auto event:{"BluetoothAdapterStateChange","BluetoothDeviceFound","BLEConnectionStateChange","BLECharacteristicValueChange"})add_event("wx.on"+std::string(event),"wx.off"+std::string(event),event);
    for(auto event:{"Start","Stop","Pause","Resume","Error","InterruptionBegin"})add_event("object:RecorderManager.on"+std::string(event),"",event);
    snapshot("wx.getAppAuthorizeSetting",app_authorization());try{snapshot("wx.getBatteryInfoSync",battery());}catch(const std::exception&){}

  }
  ~MacDeviceApis() override{retire();}
  Status set_native_parent(void* parent) override {
    options_.native_parent=parent;if(!parent){for(auto& entry:mounts_)destroy_mount(entry.second);mounts_.clear();std::lock_guard<std::mutex> lock(media_->mutex);media_->targets.clear();media_->latest.clear();}return {};
  }
  Status pump() override {
    auto status=DeviceBase::pump();if(!status||queue_->closed)return status;
    if(location_overflow_&&location_overflow_->exchange(false))location_backpressure();
    if(bluetooth_overflow_&&bluetooth_overflow_->exchange(false)){close_bluetooth();bluetooth_terminal_delivery_=std::make_shared<std::atomic<bool>>(true);DeviceQueued event{DeviceQueued::NativeTerminalEvent,"","BluetoothAdapterStateChange","",{{"available",false},{"discovering",false},{"errMsg","native Bluetooth ingress backpressure limit exceeded"}}};event.delivery_allowed=bluetooth_terminal_delivery_;queue_->push(std::move(event));}
    for(auto item=bluetooth_open_intents_.begin();item!=bluetooth_open_intents_.end();)if(!calls_.count(item->first))item=bluetooth_open_intents_.erase(item);else ++item;
    photos_.erase(std::remove_if(photos_.begin(),photos_.end(),[](WCDevicePhoto* photo){return photo->finished&&photo->finished->load();}),photos_.end());dialogs_.erase(std::remove_if(dialogs_.begin(),dialogs_.end(),[](NSWindow* window){return !window.sheetParent&&!window.visible;}),dialogs_.end());
    auto authorization=app_authorization();if(authorization!=snapshots_["wx.getAppAuthorizeSetting"]){snapshots_["wx.getAppAuthorizeSetting"]=authorization;DeviceQueued update;update.kind=DeviceQueued::Snapshots;update.value={{"wx.getAppAuthorizeSetting",authorization}};queue_->push(std::move(update));}
    auto now=std::chrono::steady_clock::now();
    for(auto entry=location_intents_.begin();entry!=location_intents_.end();)if(!calls_.count(entry->first))entry=location_intents_.erase(entry);else ++entry;
    for(auto entry=location_calls_.begin();entry!=location_calls_.end();)if(now>=entry->second.deadline){auto call=entry->first;*entry->second.delivery=false;entry=location_calls_.erase(entry);if(calls_.count(call))fail(call,calls_.at(call).context.value("api","wx.getLocation"),"OS location fix timed out","timeout");}else ++entry;
    for(auto entry=ble_pending_.begin();entry!=ble_pending_.end();)if(now>=entry->second.deadline){auto pending=entry->second;if(pending.operation=="connect")[bluetooth_ cancelPeripheralConnection:peripherals_.at(entry->first)];entry=ble_pending_.erase(entry);if(calls_.count(pending.call))fail(pending.call,pending.api,"Bluetooth operation timed out","timeout");}else ++entry;
    if(foreground_)for(auto& entry:mounts_){auto& mount=entry.second;if(!mount.video_output||!mount.player||!mount.snapshot.value("visible",false))continue;auto time=[mount.video_output itemTimeForHostTime:CACurrentMediaTime()];if([mount.video_output hasNewPixelBufferForItemTime:time]){CVPixelBufferRef buffer=[mount.video_output copyPixelBufferForItemTime:time itemTimeForDisplay:nil];if(buffer){media_pixels(weak_queue(),media_,entry.first,mount.incarnation,buffer,media_context_);CVPixelBufferRelease(buffer);}}}
    std::map<std::string,Json> frames;{std::lock_guard<std::mutex> lock(media_->mutex);frames.swap(media_->latest);}
    for(auto& frame:frames){auto result=command_("hostComponentFrame",std::get<Value::Object>(decode(frame.second).data),nullptr);if(!result)return result;}return {};
  }
  Status update_components(const Value::Array& values) override {
    try {auto nodes=encode(Value(values));std::set<std::string> keep;
      for(const auto& node:nodes){auto tag=node.value("tag","");if(tag!="camera"&&tag!="video")continue;auto id=node.at("nodeId").get<std::string>();keep.insert(id);auto found=mounts_.find(id);if(found==mounts_.end()){if(mounts_.size()>=16)throw std::invalid_argument("native media component budget exceeded");Mount mount;mount.incarnation=++mount_sequence_;mounts_[id]=mount;found=mounts_.find(id);}auto& mount=found->second;
        if(!mount.snapshot.is_null()&&mount.snapshot.value("tag","")!=tag){destroy_mount(mount);mount=Mount{};mount.incarnation=++mount_sequence_;}mount.snapshot=node;
        {std::lock_guard<std::mutex> lock(media_->mutex);media_->targets[id]=node;media_->incarnations[id]=mount.incarnation;}
        bool active=node.value("visible",true)&&foreground_&&options_.native_parent;
        if(tag=="camera"){if(active){if(!mount.capture&&!mount.permission_pending)authorize_camera(id);else if(mount.capture&&!mount.capture.running)[mount.capture startRunning];}else [mount.capture stopRunning];}
        else {auto attributes=node.value("attributes",Json::object());auto src=attributes.value("src","");if(src!=mount.source){[mount.player pause];mount.player=nil;mount.video_output=nil;mount.source=src;if(!src.empty()){NSURL* url=media_source(src);mount.player=[AVPlayer new];auto item=[AVPlayerItem playerItemWithURL:url];mount.video_output=[[AVPlayerItemVideoOutput alloc]initWithPixelBufferAttributes:@{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA)}];[item addOutput:mount.video_output];[mount.player replaceCurrentItemWithPlayerItem:item];if(active&&attributes.value("autoplay","")=="true")[mount.player play];}}if(!active)[mount.player pause];}
      }
      for(auto entry=mounts_.begin();entry!=mounts_.end();)if(!keep.count(entry->first)){auto id=entry->first;destroy_mount(entry->second);entry=mounts_.erase(entry);std::lock_guard<std::mutex> lock(media_->mutex);media_->targets.erase(id);media_->incarnations.erase(id);media_->latest.erase(id);}else ++entry;return {};
    }catch(const std::exception& error){return Status::error("ERR_NATIVE_COMPONENT",error.what());}
  }
};
}  // namespace weapp::desktop::detail
namespace weapp::desktop {
std::unique_ptr<DeviceApis> DeviceApis::create(const DeviceApiOptions& options,Command command,Status& status) {
  if(!NSThread.isMainThread){status=Status::error("ERR_WRONG_THREAD","macOS device APIs require the AppKit main thread");return {};}
  try{auto result=std::make_unique<detail::MacDeviceApis>(options,std::move(command));status={};return result;}
  catch(const std::exception& error){status=Status::error("ERR_NATIVE_DEVICES",error.what());return {};}
}
}
