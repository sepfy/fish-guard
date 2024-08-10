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
#include <INIReader.h>
#include <wpa_ctrl.h>

#include "peer.h"

const std::string kDefaultConfig = "/boot/config.ini";
const int kFeederCount = 3;
const char kCameraPipeline[] = "libcamerasrc auto-focus-mode=2 ! video/x-raw, format=(string)NV12, width=(int)1920, height=(int)1080, framerate=(fraction)20/1, interlace-mode=(string)progressive, colorimetry=(string)bt709 ! videoflip method=rotate-180 ! v4l2h264enc capture-io-mode=4 output-io-mode=4 extra-controls=encode,video_bitrate=2500000,video_gop_size=300 ! video/x-h264, stream-format=(string)byte-stream, level=(string)4, alignment=(string)au ! h264parse config-interval=-1 ! queue ! appsink name=camera-sink";

#include <iostream>
#include <fstream>
#include <string>
#include <unistd.h> // for usleep

class StepperMotor {
 public:
  StepperMotor(int pin_1, int pin_2, int pin_3, int pin_4);
  void Step(int steps, int delay);

 private:
  int pin_1_, pin_2_, pin_3_, pin_4_;
 
  void SetupPins();
  void ExportPin(int pin);
  void SetDirection(int pin, std::string direction);
  void SetPinValue(int pin, int value);
  void StepMotor(int sequence);
};

StepperMotor::StepperMotor(int pin_1, int pin_2, int pin_3, int pin_4)
  : pin_1_(pin_1), pin_2_(pin_2), pin_3_(pin_3), pin_4_(pin_4) {
  SetupPins();
}

void StepperMotor::Step(int steps, int delay) {
  for (int i = 0; i < steps; i++) {
    StepMotor(0b1100);
    usleep(delay);
    StepMotor(0b0110);
    usleep(delay);
    StepMotor(0b0011);
    usleep(delay);
    StepMotor(0b1001);
    usleep(delay);
  }
}

void StepperMotor::SetupPins() {
  ExportPin(pin_1_);
  ExportPin(pin_2_);
  ExportPin(pin_3_);
  ExportPin(pin_4_);

  SetDirection(pin_1_, "out");
  SetDirection(pin_2_, "out");
  SetDirection(pin_3_, "out");
  SetDirection(pin_4_, "out");
}

void StepperMotor::ExportPin(int pin) {
  std::ofstream export_file("/sys/class/gpio/export");
  if (export_file.is_open()) {
    export_file << pin;
    export_file.close();
  } else {
    std::cerr << "Unable to export GPIO pin" << std::endl;
  }
}

void StepperMotor::SetDirection(int pin, std::string direction) {
  std::ofstream dir_file("/sys/class/gpio/gpio" + std::to_string(pin) + "/direction");
  if (dir_file.is_open()) {
    dir_file << direction;
    dir_file.close();
  } else {
    std::cerr << "Unable to set direction of GPIO pin" << std::endl;
  }
}

void StepperMotor::SetPinValue(int pin, int value) {
  std::ofstream value_file("/sys/class/gpio/gpio" + std::to_string(pin) + "/value");
  if (value_file.is_open()) {
    value_file << value;
    value_file.close();
  } else {
    std::cerr << "Unable to set value of GPIO pin" << std::endl;
  }
}

void StepperMotor::StepMotor(int sequence) {
  SetPinValue(pin_1_, (sequence & 0b1000) >> 3);
  SetPinValue(pin_2_, (sequence & 0b0100) >> 2);
  SetPinValue(pin_3_, (sequence & 0b0010) >> 1);
  SetPinValue(pin_4_, (sequence & 0b0001));
}

class FishGaurd {
 public:
  FishGaurd();
  ~FishGaurd();
  void Run(std::string device_id);

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
  StepperMotor motor_;
};

FishGaurd* FishGaurd::instance_ = nullptr;

FishGaurd::FishGaurd() : interrupted_(false), pc_(nullptr), state_(PEER_CONNECTION_CLOSED), camera_pipeline_(nullptr), camera_sink_(nullptr), is_open_(false), motor_(23, 24, 25, 8), feeder_(0) {
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

void FishGaurd::Run(std::string device_id) {

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

    if (feeder_.load() > 0) {
      feeder_.fetch_sub(1);
      motor_.Step(1, 4000);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

void WpaCtrlRequest(struct wpa_ctrl *ctrl_conn, std::string cmd) {

  char reply[128];
  size_t len = sizeof(reply);
  wpa_ctrl_request(ctrl_conn, cmd.c_str(), cmd.length(), reply, &len, NULL);
  usleep(100*1000);
}

int main(int argc, char *argv[]) {
  FishGaurd fish_guard;

  INIReader reader(kDefaultConfig);
  if (reader.ParseError() < 0) {
    std::cerr << "Can't load " << kDefaultConfig << std::endl;
    return 1;
  }

  std::string ssid = reader.Get("fish-guard", "ssid", "myssid");
  std::string password = reader.Get("fish-guard", "password", "mypassword");
  std::string device_id = reader.Get("fish-guard", "id", "mydevice");
  std::cout << "ssid: " << ssid << ", password: " << password << ", device_id: " << device_id << std::endl;

  struct wpa_ctrl *ctrl_conn = wpa_ctrl_open("/var/run/wpa_supplicant/wlan0");
  if (ctrl_conn == NULL) {
    std::cerr << "ERROR\n";
  }
  std::string cmd;
  cmd = "ADD_NETWORK";
  WpaCtrlRequest(ctrl_conn, cmd);
  cmd = "SET_NETWORK 0 ssid \"" + ssid + "\"";
  WpaCtrlRequest(ctrl_conn, cmd);
  cmd = "SET_NETWORK 0 psk \"" + password + "\"";
  WpaCtrlRequest(ctrl_conn, cmd);
  cmd = "ENABLE_NETWORK 0";
  WpaCtrlRequest(ctrl_conn, cmd);

  fish_guard.Run(device_id);
  return 0;
}

