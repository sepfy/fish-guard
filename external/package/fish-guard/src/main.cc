#include <iostream>
#include <cstring>
#include <thread>
#include <atomic>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <gst/gst.h>
#include <fstream>
#include "peer.h"

const int kFeederCount = 5;
const char kCameraPipeline[] = "libcamerasrc auto-focus-mode=2 ! video/x-raw, format=(string)NV12, width=(int)1920, height=(int)1080, framerate=(fraction)20/1, interlace-mode=(string)progressive, colorimetry=(string)bt709 ! v4l2h264enc capture-io-mode=4 output-io-mode=4 extra-controls=encode,video_bitrate=2500000,video_gop_size=300 ! video/x-h264, stream-format=(string)byte-stream, level=(string)4, alignment=(string)au ! h264parse config-interval=-1 ! appsink name=camera-sink";

class PwmHandler {
 public:
  PwmHandler();
  void SetDutyCycle(int duty_cycle);
  void EnablePwm(bool enable);
 private:
  void ExportPwm();
  void SetPeriod(int period);
};

PwmHandler::PwmHandler() {
  ExportPwm();
  SetPeriod(20000000); // 20ms period for 50Hz PWM signal
  SetDutyCycle(5000000);
}

void PwmHandler::EnablePwm(bool enable) {
  const std::string pwm_enable_path = "/sys/class/pwm/pwmchip0/pwm0/enable";
  std::ofstream enable_file(pwm_enable_path);
  if (enable_file.is_open()) {
    enable_file << (enable ? "1" : "0");
    enable_file.close();
    std::cout << "PWM " << (enable ? "enabled" : "disabled") << std::endl;
  } else {
    std::cerr << "Failed to open " << pwm_enable_path << std::endl;
  }
}

void PwmHandler::ExportPwm() {
  const std::string export_path = "/sys/class/pwm/pwmchip0/export";
  std::ofstream export_file(export_path);
  if (export_file.is_open()) {
    export_file << "0";
    export_file.close();
    std::cout << "Exported PWM0" << std::endl;
  } else {
    std::cerr << "Failed to open " << export_path << std::endl;
  }
}

void PwmHandler::SetPeriod(int period) {
  const std::string pwm_period_path = "/sys/class/pwm/pwmchip0/pwm0/period";
  std::ofstream period_file(pwm_period_path);
  if (period_file.is_open()) {
    period_file << period;
    period_file.close();
    std::cout << "Set PWM period to " << period << std::endl;
  } else {
    std::cerr << "Failed to open " << pwm_period_path << std::endl;
  }
}

void PwmHandler::SetDutyCycle(int duty_cycle) {
  const std::string pwm_duty_path = "/sys/class/pwm/pwmchip0/pwm0/duty_cycle";
  std::ofstream duty_file(pwm_duty_path);
  if (duty_file.is_open()) {
    duty_file << duty_cycle;
    duty_file.close();
    std::cout << "Set PWM duty cycle to " << duty_cycle << std::endl;
  } else {
    std::cerr << "Failed to open " << pwm_duty_path << std::endl;
  }
}

class FishGaurd {
 public:
  FishGaurd();
  ~FishGaurd();
  void Run();

 private:
  static void OnConnectionStateChange(PeerConnectionState state, void *data);
  static uint64_t GetEpoch();
  static GstFlowReturn OnVideoData(GstElement *sink, void *data);
  static void OnOpen(void *user_data);
  static void OnClose(void *user_data);
  static void OnMessage(char *msg, size_t len, void *user_data);
  static void OnRequestKeyframe();
  static void SignalHandler(int signal);
  void PeerSignalingTask();
  void PeerConnectionTask();
  std::string GetHwAddr(const std::string &iface);

  static FishGaurd* instance_;
  std::atomic<int> feeder_;
  std::atomic<bool> interrupted_;
  PeerConnection *pc_;
  PeerConnectionState state_;
  GstElement *camera_pipeline_;
  GstElement *camera_sink_;

  std::thread peer_signaling_thread_;
  std::thread peer_connection_thread_;

  PeerConfiguration config_ = {
    .ice_servers = {
      { .urls = "stun:stun.l.google.com:19302" },
    },
    .audio_codec = CODEC_PCMA,
    .video_codec = CODEC_H264,
    .datachannel = DATA_CHANNEL_STRING,
    .on_request_keyframe = OnRequestKeyframe
  };

  ServiceConfiguration service_config_ = SERVICE_CONFIG_DEFAULT();

  bool is_open_;
  PwmHandler pwm_handler_;
};

FishGaurd* FishGaurd::instance_ = nullptr;

FishGaurd::FishGaurd() : interrupted_(false), pc_(nullptr), state_(PEER_CONNECTION_CLOSED), camera_pipeline_(nullptr), camera_sink_(nullptr), is_open_(false), pwm_handler_(), feeder_(0) {
  gst_init(nullptr, nullptr);
  signal(SIGINT, SignalHandler);
  instance_ = this;
}

FishGaurd::~FishGaurd() {
  gst_element_set_state(camera_pipeline_, GST_STATE_NULL);
  if (peer_signaling_thread_.joinable()) {
    peer_signaling_thread_.join();
  }
  if (peer_connection_thread_.joinable()) {
    peer_connection_thread_.join();
  }
  peer_signaling_leave_channel();
  peer_connection_destroy(pc_);
  peer_deinit();
}

void FishGaurd::Run() {

  std::string device_id = "fish-" + GetHwAddr("wlan0");
  std::cout << "open https://sepfy.github.io/webrtc?deviceId=" << device_id << std::endl;

  camera_pipeline_ = gst_parse_launch(kCameraPipeline, nullptr);
  camera_sink_ = gst_bin_get_by_name(GST_BIN(camera_pipeline_), "camera-sink");
  g_signal_connect(camera_sink_, "new-sample", G_CALLBACK(OnVideoData), nullptr);
  g_object_set(camera_sink_, "emit-signals", TRUE, nullptr);

  peer_init();
  pc_ = peer_connection_create(&config_);
  peer_connection_oniceconnectionstatechange(pc_, OnConnectionStateChange);
  peer_connection_ondatachannel(pc_, OnMessage, OnOpen, OnClose);

  service_config_.client_id = device_id.c_str();
  service_config_.pc = pc_;
  service_config_.mqtt_url = "td99649f.ala.asia-southeast1.emqxsl.com";
  service_config_.username = "aaron";
  service_config_.password = "12345678";
  peer_signaling_set_config(&service_config_);

  while (!interrupted_) {

    if (!peer_signaling_join_channel()) {
      break;
    }
    std::cout << "connect failed... retry";
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
  }

  peer_connection_thread_ = std::thread(&FishGaurd::PeerConnectionTask, this);
  peer_signaling_thread_ = std::thread(&FishGaurd::PeerSignalingTask, this);

  bool enable = false;
  while (!interrupted_) {

    if (feeder_.load() == kFeederCount) {

      if (!enable) {
        pwm_handler_.EnablePwm(true);
        enable = true;
      }
    }

    if (feeder_.load() > 0) {
      feeder_.fetch_sub(1);
    } else {

      if (enable) {
        pwm_handler_.EnablePwm(false);
	enable = false;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void FishGaurd::OnConnectionStateChange(PeerConnectionState state, void *data) {
  std::cout << "state is changed: " << state << std::endl;
  if (instance_->state_ != state) {
    instance_->state_ = state;
    switch (instance_->state_) {
      case PEER_CONNECTION_CLOSED:
        gst_element_set_state(instance_->camera_pipeline_, GST_STATE_NULL);
        break;
      case PEER_CONNECTION_COMPLETED:
        gst_element_set_state(instance_->camera_pipeline_, GST_STATE_PLAYING);
        break;
      default:
        break;
    }
  }
}

uint64_t FishGaurd::GetEpoch() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

GstFlowReturn FishGaurd::OnVideoData(GstElement *sink, void *data) {
  static int fps = 0;
  static int bytes = 0;
  static uint64_t start_ts = 0;
  uint64_t curr_ts;
  GstSample *sample;
  GstBuffer *buffer;
  GstMapInfo info;

  g_signal_emit_by_name(sink, "pull-sample", &sample);

  if (sample) {
    buffer = gst_sample_get_buffer(sample);
    gst_buffer_map(buffer, &info, GST_MAP_READ);
    peer_connection_send_video(instance_->pc_, info.data, info.size);

    gst_buffer_unmap(buffer, &info);
    gst_sample_unref(sample);

    bytes += info.size;
    fps++;
    curr_ts = GetEpoch();
    if (curr_ts - start_ts > 5000) {
      std::cout << "fps: " << (float)fps * 1000 / (curr_ts - start_ts) << ", bps: " << (float)bytes * 8 * 1000 / (curr_ts - start_ts) << std::endl;
      bytes = 0;
      fps = 0;
      start_ts = curr_ts;
    }

    return GST_FLOW_OK;
  }

  return GST_FLOW_ERROR;
}

void FishGaurd::OnOpen(void *user_data) {
  instance_->is_open_ = true;
}

void FishGaurd::OnClose(void *user_data) {
  instance_->is_open_ = false;
}

void FishGaurd::OnMessage(char *msg, size_t len, void *user_data) {
  //std::cout << "on message: " << msg << std::endl;
  instance_->feeder_.store(kFeederCount);
}

void FishGaurd::OnRequestKeyframe() {
  std::cout << "request keyframe" << std::endl;
}

void FishGaurd::SignalHandler(int signal) {
  instance_->interrupted_ = true;
}

void FishGaurd::PeerSignalingTask() {
  while (!interrupted_) {
    peer_signaling_loop();
    usleep(1000);
  }
}

void FishGaurd::PeerConnectionTask() {
  while (!interrupted_) {
    peer_connection_loop(pc_);
    usleep(1000);
  }
}

std::string FishGaurd::GetHwAddr(const std::string &iface) {
  uint8_t *ptr;
  int fd;
  struct ifreq ifr;
  std::string hwaddr;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return hwaddr;

  ifr.ifr_addr.sa_family = AF_INET;
  strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);

  if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
    ptr = (uint8_t *)&ifr.ifr_ifru.ifru_hwaddr.sa_data[0];
    char buf[32];
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x", *ptr, *(ptr + 1), *(ptr + 2), *(ptr + 3), *(ptr + 4), *(ptr + 5));
    hwaddr = buf;
  }

  close(fd);
  return hwaddr;
}

int main(int argc, char *argv[]) {
  FishGaurd fish_guard;
  fish_guard.Run();
  return 0;
}

