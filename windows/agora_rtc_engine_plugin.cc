#pragma warning(disable: 4302)
#pragma warning(disable: 4311)

#include "include/agora_rtc_engine/agora_rtc_engine_plugin.h"
#include "include/agora_rtc_engine/call_api_method_call_handler.h"

// This must be included before many other Windows headers.
#include <windows.h>

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/texture_registrar.h>

#include <map>
#include <memory>
#include <sstream>
#include <vector>
#include <string>
#include <filesystem>

#include "include/agora_rtc_engine/agora_rtc_channel_plugin.h"
#include "include/agora_rtc_engine/agora_rtc_device_manager_plugin.h"
#include "include/agora_rtc_engine/agora_texture_view_factory.h"
#include "iris_rtc_engine.h"
#include "iris_rtc_raw_data.h"
#include "iris_video_processor.h"

#include "include/raw_engine/IAgoraRtcEngine.h"

namespace
{
  using namespace flutter;
  using namespace agora::iris;
  using namespace agora::iris::rtc;

  class EventHandler;

  class AgoraRtcEnginePlugin : public Plugin
  {
  public:
    static void RegisterWithRegistrar(PluginRegistrarWindows *registrar);

    AgoraRtcEnginePlugin(PluginRegistrar *registrar);

    virtual ~AgoraRtcEnginePlugin();

    EventSink<EncodableValue> *event_sink();

  private:
    IrisRtcEngine *engine(EncodableMap &arguments);

    // Called when a method is called on this plugin's channel from Dart.
    void HandleMethodCall(const MethodCall<EncodableValue> &method_call,
                          std::unique_ptr<MethodResult<EncodableValue>> result);

  private:
    std::unique_ptr<EventSink<EncodableValue>> event_sink_;
    std::unique_ptr<IrisRtcEngine> engine_main_;
    std::unique_ptr<IrisRtcEngine> engine_sub_;
    std::unique_ptr<EventHandler> handler_main_;
    std::unique_ptr<EventHandler> handler_sub_;
    std::unique_ptr<CallApiMethodCallHandler> callApiMethodCallHandlerMain_;
    std::unique_ptr<CallApiMethodCallHandler> callApiMethodCallHandlerSub_;
    std::unique_ptr<AgoraTextureViewFactory> factory_;
    std::unique_ptr<IrisVideoFrameBufferManager> videoFrameBufferManagerMain_;
    std::unique_ptr<IrisVideoFrameBufferManager> videoFrameBufferManagerSub_;
  };

  class EventHandler : public IrisEventHandler
  {
  public:
    EventHandler(AgoraRtcEnginePlugin *plugin, bool sub_process = false)
        : plugin_(plugin), sub_process_(sub_process) {}

    void OnEvent(const char *event, const char *data) override
    {
      if (plugin_->event_sink())
      {
        EncodableMap ret = {
            {EncodableValue("methodName"), EncodableValue(event)},
            {EncodableValue("data"), EncodableValue(data)},
            {EncodableValue("subProcess"), EncodableValue(sub_process_)}};
        plugin_->event_sink()->Success(ret);
      }
    }

    void OnEvent(const char *event, const char *data, const void *buffer,
                 unsigned int length) override
    {
      if (plugin_->event_sink())
      {
        std::vector<uint8_t> vector(length);
        if (buffer && length)
        {
          memcpy(&vector[0], buffer, length);
        }
        EncodableMap ret = {
            {EncodableValue("methodName"), EncodableValue(event)},
            {EncodableValue("data"), EncodableValue(data)},
            {EncodableValue("buffer"), EncodableValue(vector)},
            {EncodableValue("subProcess"), EncodableValue(sub_process_)}};
        plugin_->event_sink()->Success(ret);
      }
    }

  private:
    AgoraRtcEnginePlugin *plugin_;
    bool sub_process_;
  };
  
  template <typename T>
  static bool GetValueFromEncodableMap(const flutter::EncodableMap *map,
                                       const char *key, T &out)
  {
    auto iter = map->find(flutter::EncodableValue(key));
    if (iter != map->end() && !iter->second.IsNull())
    {
      if (auto *value = std::get_if<T>(&iter->second))
      {
        out = *value;
        return true;
      }
    }
    return false;
  }

  bool isSubProcess(EncodableMap &arguments)
  {
    if (arguments.find(EncodableValue("subProcess")) == arguments.end()) {
      return false;
    }
    auto subProcess = std::get<bool>(arguments[EncodableValue("subProcess")]);
    return subProcess;
  }

  void AgoraRtcEnginePlugin::RegisterWithRegistrar(
      PluginRegistrarWindows *registrar)
  {
    auto method_channel = std::make_unique<MethodChannel<EncodableValue>>(
        registrar->messenger(), "agora_rtc_engine",
        &StandardMethodCodec::GetInstance());
    auto event_channel = std::make_unique<EventChannel<EncodableValue>>(
        registrar->messenger(), "agora_rtc_engine/events",
        &StandardMethodCodec::GetInstance());

    auto plugin = std::make_unique<AgoraRtcEnginePlugin>(registrar);

    method_channel->SetMethodCallHandler(
        [plugin_pointer = plugin.get()](const auto &call, auto result)
        {
          plugin_pointer->HandleMethodCall(call, std::move(result));
        });

    auto handler = std::make_unique<StreamHandlerFunctions<EncodableValue>>(
        [plugin_pointer =
             plugin.get()](const EncodableValue *arguments,
                           std::unique_ptr<EventSink<EncodableValue>> &&events)
            -> std::unique_ptr<StreamHandlerError<EncodableValue>>
        {
          plugin_pointer->event_sink_ = std::move(events);
          return nullptr;
        },
        [plugin_pointer = plugin.get()](const EncodableValue *arguments)
            -> std::unique_ptr<StreamHandlerError<EncodableValue>>
        {
          plugin_pointer->event_sink_ = nullptr;
          return nullptr;
        });
    event_channel->SetStreamHandler(std::move(handler));

    AgoraRtcChannelPluginRegisterWithRegistrar(registrar,
                                               plugin->engine_main_.get());
    AgoraRtcDeviceManagerPluginRegisterWithRegistrar(registrar,
                                                     plugin->engine_main_.get());

    registrar->AddPlugin(std::move(plugin));
  }

  AgoraRtcEnginePlugin::AgoraRtcEnginePlugin(PluginRegistrar *registrar)
      : engine_main_(new IrisRtcEngine),
        engine_sub_(new IrisRtcEngine(nullptr, kEngineTypeSubProcess)),
        handler_main_(new EventHandler(this)),
        handler_sub_(new EventHandler(this, true)),
        factory_(new AgoraTextureViewFactory(registrar)),
        videoFrameBufferManagerMain_(new IrisVideoFrameBufferManager()),
        videoFrameBufferManagerSub_(new IrisVideoFrameBufferManager())
  {

    engine_main_->SetEventHandler(handler_main_.get());
    engine_sub_->SetEventHandler(handler_sub_.get());
    engine_main_->raw_data()->Attach(videoFrameBufferManagerMain_.get());
    engine_sub_->raw_data()->Attach(videoFrameBufferManagerSub_.get());
    callApiMethodCallHandlerMain_ = std::make_unique<CallApiMethodCallHandler>(engine_main_.get());
    callApiMethodCallHandlerSub_ = std::make_unique<CallApiMethodCallHandler>(engine_sub_.get());
  }

  AgoraRtcEnginePlugin::~AgoraRtcEnginePlugin() {
     factory_.reset();
     videoFrameBufferManagerMain_.reset();
     videoFrameBufferManagerSub_.reset();
  }

  EventSink<EncodableValue> *AgoraRtcEnginePlugin::event_sink()
  {
    return event_sink_.get();
  }

  IrisRtcEngine *AgoraRtcEnginePlugin::engine(EncodableMap &arguments)
  {
    if (isSubProcess(arguments))
    {
      return engine_sub_.get();
    }
    else
    {
      return engine_main_.get();
    }
  }

  void AgoraRtcEnginePlugin::HandleMethodCall(
      const MethodCall<EncodableValue> &method_call,
      std::unique_ptr<MethodResult<EncodableValue>> result)
  {
    auto method = method_call.method_name();
    if (!method_call.arguments())
    {
      result->Error("Bad Arguments", "Null arguments received");
      return;
    }

    if (method.compare("createTextureRender") == 0)
    {
      auto arguments = std::get<EncodableMap>(*method_call.arguments());
      auto texture_id = factory_->CreateTextureRenderer(
          engine(arguments)->raw_data()->buffer_manager());
      result->Success(EncodableValue(texture_id));
    }
    else if (method.compare("getScreenShareSources") == 0)
    {
      auto engine = reinterpret_cast<agora::rtc::IRtcEngine *>(engine_main_->rtc_engine());
      SIZE size;
      size.cx = 225 * 2;
      size.cy = 116 * 2;
      auto infos = engine->getScreenCaptureSources(size, size, true);
      std::vector<flutter::EncodableValue> resultData;
      for (unsigned int i = 0; i < infos->getCount(); i++) {
        agora::rtc::ScreenCaptureSourceInfo info = infos->getSourceInfo(i);
        auto thumbImage = info.thumbImage;
        std::vector<uint8_t> vimgThumb(thumbImage.buffer, thumbImage.buffer + thumbImage.length);

        auto iconImage = info.iconImage;
        std::vector<uint8_t> vimgIcon(iconImage.buffer, iconImage.buffer + iconImage.length);

        std::map<flutter::EncodableValue, flutter::EncodableValue> item;
        
        item.insert(std::map<flutter::EncodableValue, flutter::EncodableValue> ::value_type(flutter::EncodableValue("id"), (int32_t)info.sourceId));
        item.insert(std::map<flutter::EncodableValue, flutter::EncodableValue> ::value_type(flutter::EncodableValue("thumb"), flutter::EncodableValue(vimgThumb)));
        item.insert(std::map<flutter::EncodableValue, flutter::EncodableValue> ::value_type(flutter::EncodableValue("thumbWidth"), (int32_t)thumbImage.width));
        item.insert(std::map<flutter::EncodableValue, flutter::EncodableValue> ::value_type(flutter::EncodableValue("thumbHeight"), (int32_t)thumbImage.height));
        item.insert(std::map<flutter::EncodableValue, flutter::EncodableValue> ::value_type(flutter::EncodableValue("icon"), flutter::EncodableValue(vimgIcon)));
        item.insert(std::map<flutter::EncodableValue, flutter::EncodableValue> ::value_type(flutter::EncodableValue("iconWidth"), (int32_t)iconImage.width));
        item.insert(std::map<flutter::EncodableValue, flutter::EncodableValue> ::value_type(flutter::EncodableValue("iconHeight"), (int32_t)iconImage.height));


        resultData.emplace_back(item);
      }
      infos->release();
      result->Success(resultData);
    }
    else if (method.compare("destroyTextureRender") == 0)
    {
      const auto *arguments = std::get_if<EncodableMap>(method_call.arguments());
      if (!arguments)
      {
        result->Error("Invalid arguments", "Invalid argument type.");
        return;
      }

      int64_t texture_id;
      if (!GetValueFromEncodableMap(arguments, "id", texture_id))
      {
        result->Error("Invalid arguments", "No id provided.");
        return;
      }
      
      factory_->DestoryTextureRenderer(texture_id);
      result->Success();
    }
    else if (method.compare("getAssetAbsolutePath") == 0)
    {
      auto asset_path = std::get<std::string>(*method_call.arguments());
      char exe_path[MAX_PATH];
      GetModuleFileNameA(NULL, exe_path, MAX_PATH);

      if (exe_path)
      {
        // The real asset path: <exe path>/data/flutter_assets/<asset_path>
        auto realPath = std::filesystem::path(exe_path).parent_path() / std::filesystem::path("data") / std::filesystem::path("flutter_assets") / std::filesystem::path(asset_path);
        result->Success(EncodableValue(realPath.u8string()));
      }
    }
    else
    {
      auto arguments = std::get<EncodableMap>(*method_call.arguments());
      if (isSubProcess(arguments))
      {
        callApiMethodCallHandlerSub_->HandleMethodCall(method_call, std::move(result));
      }
      else
      {
        callApiMethodCallHandlerMain_->HandleMethodCall(method_call, std::move(result));
      }
    }
  }
} // namespace

void AgoraRtcEnginePluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar)
{
  AgoraRtcEnginePlugin::RegisterWithRegistrar(
      PluginRegistrarManager::GetInstance()
          ->GetRegistrar<PluginRegistrarWindows>(registrar));
}
