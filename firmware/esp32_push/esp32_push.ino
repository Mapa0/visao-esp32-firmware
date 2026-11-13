// ESP32-CAM AI-Thinker com modulo ESP32-S.
// Publica os JPEGs no receptor MapaZero Cam.
// O reconhecimento facial acontece no computador central, nao nesta placa.

#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_system.h>
#include "esp_camera.h"
#include <lwip/sockets.h>
#include <sys/time.h>
#include <mbedtls/gcm.h>
#if __has_include("esp32_receiver_config.h")
#include "esp32_receiver_config.h"
#else
#include "esp32_receiver_config.example.h"
#endif
#include "mapazero_ca.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#ifndef ESP32_USE_HTTPS
#define ESP32_USE_HTTPS 0
#endif

// Pinagem AI-Thinker ESP32-CAM, conforme o exemplo CameraWebServer da Espressif.
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22
#define FLASH_GPIO_NUM 4

static constexpr size_t datagramSize = 1200;
static constexpr size_t maxFrameSize = 640 * 1024;
static constexpr size_t tagSize = 16;
static constexpr uint16_t localPort = 5001;
static constexpr char localSubscription[] = "ESP32CAM_SUB1";

struct __attribute__((packed)) LocalFrameHeader {
  char magic[4];
  uint32_t sequence;
  uint32_t size;
  uint16_t fragment;
};
static_assert(sizeof(LocalFrameHeader) == 14);

struct __attribute__((packed)) FrameHeader {
  char magic[4];
  uint8_t cameraId[16];
  uint8_t timestamp[8];
  uint32_t size;
  uint16_t fragment;
  uint8_t nonce[12];
};
struct CaptureProfile {
  framesize_t resolution;
  uint32_t clockHz;
  const char* name;
};
static_assert(sizeof(FrameHeader) == 46);
static constexpr size_t fragmentSize = datagramSize - sizeof(FrameHeader) - tagSize;

static int udpSocket = -1;
static sockaddr_in localReceiver = {};
static unsigned long localSubscribedAt = 0;
static uint32_t localSequence = 0;
static bool localOnly = false, candidateLocalOnly = false;
static bool cloudHttps = ESP32_USE_HTTPS, candidateCloudHttps = ESP32_USE_HTTPS;
static uint8_t datagram[datagramSize];
static uint64_t lastTimestamp = 0;
static uint8_t cameraId[16];
static WiFiClientSecure tls;
static HTTPClient https;
static mbedtls_gcm_context cipher;
static bool cipherReady = false;
static bool senderReady = false;
static unsigned long nextSenderAttempt = 0;
static bool cameraReady = false;
static unsigned long lastCameraAttempt = 0;
static IPAddress reportedIp;
static WebServer portal(80);
static constexpr char settingsNamespace[] = "iot-demo";
static constexpr char resetCountKey[] = "rst-count";
static constexpr unsigned long resetWindowMs = 2500;
static constexpr unsigned long wifiTimeoutMs = 30000;
static constexpr unsigned long confirmationMs = 60000;
static unsigned long portalConfirmedAt = 0;
static String portalToken, candidateWifi;
static bool portalActive = false, connected = false, attempting = false;
static bool saveCandidate = false;
static const char* portalError = nullptr;
static unsigned long wifiAttemptStarted = 0;
static framesize_t requestedResolution = FRAMESIZE_QXGA;
static framesize_t candidateResolution = FRAMESIZE_QXGA;
static const framesize_t resolutions[] = {FRAMESIZE_QVGA, FRAMESIZE_VGA, FRAMESIZE_SVGA, FRAMESIZE_XGA, FRAMESIZE_SXGA, FRAMESIZE_UXGA, FRAMESIZE_QXGA};
static const char* const resolutionNames[] = {"QVGA (320 x 240)", "VGA (640 x 480)", "SVGA (800 x 600)", "XGA (1024 x 768)", "SXGA (1280 x 1024)", "UXGA (1600 x 1200)", "QXGA (2048 x 1536)"};

static void stopUdp() {
  if (udpSocket >= 0) close(udpSocket);
  udpSocket = -1;
  localReceiver = {};
  https.end();
  tls.stop();
  senderReady = false;
}

static void stopVideo() {
  stopUdp();
  reportedIp = IPAddress();
  nextSenderAttempt = 0;
}

static String videoUrl(const String& host) {
  if (localOnly) return "udp://" + host + ":" + String(localPort);
  if (cloudHttps) return String("https://") + ESP32_RECEIVER_HOST;
  return String("udp://") + ESP32_RECEIVER_HOST + ":" + ESP32_RECEIVER_PORT;
}

static bool readHex(const char* value, uint8_t* output, size_t size) {
  if (strlen(value) != size * 2) return false;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < size; ++i) {
    const int high = digit(value[i * 2]);
    const int low = digit(value[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    output[i] = high * 16 + low;
  }
  return true;
}

static bool startSender() {
  if (localOnly) {
    udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(localPort);
    address.sin_addr.s_addr = static_cast<uint32_t>(WiFi.localIP());
    if (udpSocket >= 0 && bind(udpSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return true;
    stopUdp();
    return false;
  }
  uint8_t key[32];
  if (!readHex(ESP32_CAMERA_ID, cameraId, sizeof(cameraId)) ||
      !readHex(ESP32_PUBLISH_KEY, key, sizeof(key))) {
    Serial.println("ID ou chave de publicacao invalida.");
    return false;
  }
  if (cloudHttps) {
#if ESP32_USE_HTTPS
    tls.setCACert(ESP32_ROOT_CA);
#else
    tls.setCACert(mapazeroRootCA);
#endif
    https.setConnectTimeout(3000);
    https.setTimeout(3000);
    https.setReuse(true);
    return https.begin(tls, String("https://") + ESP32_RECEIVER_HOST +
                            "/api/esp32/" + ESP32_CAMERA_ID + "/frame");
  }
  if (!cipherReady) {
    mbedtls_gcm_init(&cipher);
    if (mbedtls_gcm_setkey(&cipher, MBEDTLS_CIPHER_ID_AES, key, 256) != 0) {
      mbedtls_gcm_free(&cipher);
      return false;
    }
    cipherReady = true;
  }
  IPAddress destination;
  if (WiFi.hostByName(ESP32_RECEIVER_HOST, destination) != 1) return false;
  udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(ESP32_RECEIVER_PORT);
  address.sin_addr.s_addr = static_cast<uint32_t>(destination);
  if (udpSocket < 0 || connect(udpSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    stopUdp();
    return false;
  }
  return true;
}

static void updateLocalReceiver() {
  if (millis() - localSubscribedAt >= 3500) localReceiver = {};
  sockaddr_in candidate = {};
  socklen_t length = sizeof(candidate);
  char request[sizeof(localSubscription)];
  const int count = recvfrom(udpSocket, request, sizeof(request), MSG_DONTWAIT,
                             reinterpret_cast<sockaddr*>(&candidate), &length);
  if (count != sizeof(localSubscription) - 1 || memcmp(request, localSubscription, count) != 0) return;
  const IPAddress client(candidate.sin_addr.s_addr);
  const IPAddress mask = WiFi.subnetMask();
  if ((static_cast<uint32_t>(client) & static_cast<uint32_t>(mask)) !=
      (static_cast<uint32_t>(WiFi.localIP()) & static_cast<uint32_t>(mask))) return;
  if (!localReceiver.sin_port || (localReceiver.sin_addr.s_addr == candidate.sin_addr.s_addr &&
                                  localReceiver.sin_port == candidate.sin_port)) {
    localReceiver = candidate;
    localSubscribedAt = millis();
  }
}

static void sendLocalFrame(const camera_fb_t* frame) {
  LocalFrameHeader header = {{'E', 'C', 'J', '1'}, htonl(++localSequence), htonl(frame->len), 0};
  constexpr size_t localFragmentSize = datagramSize - sizeof(header);
  for (size_t offset = 0; offset < frame->len; offset += localFragmentSize) {
    const size_t count = min(localFragmentSize, frame->len - offset);
    header.fragment = htons(offset / localFragmentSize);
    memcpy(datagram, &header, sizeof(header));
    memcpy(datagram + sizeof(header), frame->buf + offset, count);
    if (sendto(udpSocket, datagram, sizeof(header) + count, MSG_DONTWAIT,
               reinterpret_cast<sockaddr*>(&localReceiver), sizeof(localReceiver)) < 0) break;
  }
}

static void sendCloudUdpFrame(const camera_fb_t* frame) {
  FrameHeader header = {{'E', 'C', 'J', '2'}, {}, {}, htonl(frame->len), 0, {}};
  memcpy(header.cameraId, cameraId, sizeof(cameraId));
  for (size_t i = 0; i < sizeof(header.timestamp); ++i) {
    header.timestamp[i] = lastTimestamp >> (8 * (sizeof(header.timestamp) - 1 - i));
  }
  for (size_t offset = 0; offset < frame->len; offset += fragmentSize) {
    const size_t count = min(fragmentSize, frame->len - offset);
    header.fragment = htons(offset / fragmentSize);
    esp_fill_random(header.nonce, sizeof(header.nonce));
    memcpy(datagram, &header, sizeof(header));
    uint8_t* ciphertext = datagram + sizeof(header);
    if (mbedtls_gcm_crypt_and_tag(&cipher, MBEDTLS_GCM_ENCRYPT, count, header.nonce, sizeof(header.nonce),
                                  datagram, sizeof(header), frame->buf + offset, ciphertext,
                                  tagSize, ciphertext + count) != 0 ||
        send(udpSocket, datagram, sizeof(header) + count + tagSize, MSG_DONTWAIT) < 0) break;
  }
}

static void sendFrame() {
  timeval now = {};
  if (!localOnly) {
    gettimeofday(&now, nullptr);
    if (now.tv_sec < 1700000000) {
      delay(100);
      return;
    }
  }
  camera_fb_t* frame = esp_camera_fb_get();
  if (!frame) return;
  if (frame->len <= maxFrameSize && localOnly) {
    sendLocalFrame(frame);
  } else if (frame->len <= maxFrameSize) {
    const uint64_t timestamp = static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
    lastTimestamp = max(lastTimestamp + 1, timestamp);
    if (cloudHttps) {
      https.addHeader("Content-Type", "image/jpeg");
      https.addHeader("Authorization", String("Bearer ") + ESP32_PUBLISH_KEY);
      https.addHeader("X-Frame-Time", String(static_cast<unsigned long long>(lastTimestamp)));
      const int status = https.POST(frame->buf, frame->len);
      if (status != 204) {
        Serial.printf("Falha no envio HTTPS (%d)\n", status);
        stopUdp();
        nextSenderAttempt = millis() + 2000;
      }
    } else {
      sendCloudUdpFrame(frame);
    }
  }
  esp_camera_fb_return(frame);
}

static bool tryCamera(camera_config_t& camera, const CaptureProfile& profile) {
  camera.frame_size = profile.resolution;
  camera.xclk_freq_hz = profile.clockHz;
  Serial.printf("Teste %s, XCLK %u MHz, %u buffer(s)\n", profile.name,
                static_cast<unsigned>(profile.clockHz / 1000000),
                static_cast<unsigned>(camera.fb_count));

  const esp_err_t error = esp_camera_init(&camera);
  if (error != ESP_OK) {
    Serial.printf("Falha ao iniciar camera: %s (0x%x)\n", esp_err_to_name(error), error);
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor && sensor->id.PID == OV3660_PID) {
    sensor->set_vflip(sensor, 1);
    sensor->set_brightness(sensor, 1);
    sensor->set_saturation(sensor, -2);
    // OV3660 preset 3 fixes the white-balance gains for office lighting.
    constexpr int officeWhiteBalance = 3;
    if (sensor->set_wb_mode(sensor, officeWhiteBalance) == 0) {
      Serial.println("OV3660: balanco de branco fixo (escritorio)");
    } else {
      Serial.println("OV3660: falha ao aplicar balanco de branco fixo");
      sensor->set_wb_mode(sensor, 0);
    }
  }

  for (unsigned sample = 0; sample < 3; ++sample) {
    camera_fb_t* frame = esp_camera_fb_get();
    if (!frame) break;
    const size_t size = frame->len;
    const unsigned width = frame->width;
    const unsigned height = frame->height;
    const bool valid = size >= 4 && size <= maxFrameSize && frame->buf[0] == 0xff &&
                       frame->buf[1] == 0xd8 && frame->buf[size - 2] == 0xff &&
                       frame->buf[size - 1] == 0xd9;
    esp_camera_fb_return(frame);
    if (!valid) break;
    if (sample == 2) {
      Serial.printf("Camera inicializada: PID 0x%04x\n", sensor ? sensor->id.PID : 0);
      Serial.printf("JPEG OK: %ux%u, %u bytes, 3 quadros de teste\n", width, height,
                    static_cast<unsigned>(size));
      return true;
    }
  }

  Serial.println("Perfil rejeitado: faltaram 3 JPEGs completos");
  esp_camera_deinit();
  return false;
}

static bool startCamera() {
  camera_config_t camera = {};
  camera.ledc_channel = LEDC_CHANNEL_0;
  camera.ledc_timer = LEDC_TIMER_0;
  camera.pin_d0 = Y2_GPIO_NUM;
  camera.pin_d1 = Y3_GPIO_NUM;
  camera.pin_d2 = Y4_GPIO_NUM;
  camera.pin_d3 = Y5_GPIO_NUM;
  camera.pin_d4 = Y6_GPIO_NUM;
  camera.pin_d5 = Y7_GPIO_NUM;
  camera.pin_d6 = Y8_GPIO_NUM;
  camera.pin_d7 = Y9_GPIO_NUM;
  camera.pin_xclk = XCLK_GPIO_NUM;
  camera.pin_pclk = PCLK_GPIO_NUM;
  camera.pin_vsync = VSYNC_GPIO_NUM;
  camera.pin_href = HREF_GPIO_NUM;
  camera.pin_sccb_sda = SIOD_GPIO_NUM;
  camera.pin_sccb_scl = SIOC_GPIO_NUM;
  camera.pin_pwdn = PWDN_GPIO_NUM;
  camera.pin_reset = RESET_GPIO_NUM;
  camera.pixel_format = PIXFORMAT_JPEG;
  camera.jpeg_quality = 12;

  const bool hasPsram = psramFound();
  Serial.printf("PSRAM: %u KiB detectados, %u KiB livres\n",
                static_cast<unsigned>(ESP.getPsramSize() / 1024),
                static_cast<unsigned>(ESP.getFreePsram() / 1024));
  if (hasPsram) {
    camera.fb_count = 2;
    camera.fb_location = CAMERA_FB_IN_PSRAM;
    camera.grab_mode = CAMERA_GRAB_LATEST;
    const CaptureProfile profiles[] = {
      {FRAMESIZE_QXGA, 20000000, "QXGA"},
      {FRAMESIZE_QXGA, 16000000, "QXGA"},
      {FRAMESIZE_QXGA, 10000000, "QXGA"},
      {FRAMESIZE_UXGA, 16000000, "UXGA"},
      {FRAMESIZE_SXGA, 16000000, "SXGA"},
      {FRAMESIZE_XGA, 16000000, "XGA"},
      {FRAMESIZE_SVGA, 16000000, "SVGA"},
      {FRAMESIZE_VGA, 20000000, "VGA"},
      {FRAMESIZE_VGA, 16000000, "VGA"},
      {FRAMESIZE_VGA, 10000000, "VGA"},
    };
    for (const auto& profile : profiles) {
      if (profile.resolution > requestedResolution) continue;
      if (tryCamera(camera, profile)) return true;
      delay(250);
    }
  }

  camera.fb_count = 1;
  camera.fb_location = CAMERA_FB_IN_DRAM;
  camera.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  if (tryCamera(camera, {FRAMESIZE_QVGA, 20000000, "QVGA"})) return true;
  for (int attempt = 0; attempt < 3; ++attempt) {
    delay(400);
    if (tryCamera(camera, {FRAMESIZE_QVGA, 10000000, "QVGA"})) return true;
  }
  return false;
}

static bool resetRequested(Preferences& settings) {
  const auto reason = esp_reset_reason();
  // No ESP32, o botão EN/RST também pode aparecer como reset de alimentação.
  if (reason != ESP_RST_POWERON && reason != ESP_RST_EXT) {
    if (settings.isKey(resetCountKey)) settings.remove(resetCountKey);
    return false;
  }
  const uint8_t previous = settings.getUChar(resetCountKey, 0);
  if (previous == 2) {
    if (settings.clear()) {
      Serial.println("Configuracao apagada por 3 resets. Abrindo AP.");
      return true;
    }
    settings.remove(resetCountKey);
    Serial.println("Falha ao apagar a configuracao.");
    return false;
  }
  const uint8_t count = previous == 1 ? 2 : 1;
  if (settings.putUChar(resetCountKey, count) != sizeof(count)) return false;
  Serial.printf("Reset %u/3: aperte RST novamente em ate 2,5 s para abrir o AP.\n", count);
  // A janela termina antes de iniciar o Wi-Fi ou qualquer operação da câmera.
  delay(resetWindowMs);
  settings.remove(resetCountKey);
  return false;
}

static void startPortal() {
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06lx", static_cast<unsigned long>(ESP.getEfuseMac() & 0xffffff));
  const String apName = "ESP32-CAM-" + String(suffix);
  WiFi.mode(WIFI_AP);
  portalActive = WiFi.softAP(apName.c_str());
  if (portalActive) {
    portalToken = String(esp_random(), HEX) + String(esp_random(), HEX);
    portal.begin();
    Serial.printf("AP %s (aberto) | http://%s\n",
                  apName.c_str(), WiFi.softAPIP().toString().c_str());
  } else {
    WiFi.mode(WIFI_OFF);
    Serial.println("Falha ao abrir o AP. Repita os 3 resets.");
  }
}

static void serviceFlash() {
  static unsigned long lastChange = 0;
  static unsigned long previousInterval = 0;
  static bool lit = false;
  const unsigned long interval = attempting ? 500 : portalActive && !connected ? 125 : 0;
  const auto now = millis();
  if (interval != previousInterval) {
    previousInterval = interval;
    lit = interval != 0;
  } else if (interval && now - lastChange >= interval) {
    lit = !lit;
  } else {
    return;
  }
  lastChange = now;
  digitalWrite(FLASH_GPIO_NUM, lit ? HIGH : LOW);
}

static void flashConnected() {
  for (unsigned i = 0; i < 2; ++i) {
    digitalWrite(FLASH_GPIO_NUM, HIGH);
    delay(90);
    digitalWrite(FLASH_GPIO_NUM, LOW);
    if (i == 0) delay(90);
  }
}

static bool validWifi(const String& settings) {
  const int separator = settings.indexOf('\n');
  return separator >= 1 && separator <= 32 && settings.length() - separator - 1 <= 64 &&
         settings.indexOf('\n', separator + 1) < 0 && settings.indexOf('\r') < 0 &&
         strlen(settings.c_str()) == settings.length();
}

static bool connectWifi(const String& settings, bool save) {
  if (!validWifi(settings)) return false;
  const int separator = settings.indexOf('\n');
  stopVideo();
  if (connected || attempting) WiFi.disconnect();
  connected = false;
  WiFi.mode(portalActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);
  saveCandidate = save;
  portalConfirmedAt = 0;
  candidateWifi = save ? settings : String();
  portalError = nullptr;
  attempting = true;
  wifiAttemptStarted = millis();
  serviceFlash();
  WiFi.begin(settings.substring(0, separator).c_str(), settings.substring(separator + 1).c_str());
  Serial.println("Conectando ao Wi-Fi");
  return true;
}

static String htmlText(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  return value;
}

static void handlePage() {
  String body = "<meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
                "<title>ESP32-CAM</title><style>"
                "body{color-scheme:dark;background:#111827;color:#e5edf7;font:1rem system-ui;"
                "max-width:32rem;margin:3rem auto;padding:0 1rem}"
                "label{display:block}input,select,button{display:block;width:100%;box-sizing:border-box;"
                "padding:.65rem;margin-top:.4rem;font:inherit;border:1px solid #51647e;border-radius:.4rem}"
                "button{background:#8fe0dc;color:#13202a;cursor:pointer;font-weight:600}"
                "</style><h1>ESP32-CAM</h1>";
  if (portalActive && portal.client().localIP() == WiFi.softAPIP()) {
    if (connected) {
      body += "<h2>Configuração salva</h2><p>Rede conectada: <strong>" + htmlText(WiFi.SSID()) +
              "</strong></p><p>Protocolo: <strong>" +
              String(localOnly ? "UDP local" : cloudHttps ? "HTTPS para MapaZero Cam" : "UDP para MapaZero Cam") + "</strong></p>";
      body += localOnly ? "<p>Endereço da transmissão: <code>" : "<p>Destino do envio: <code>";
      body += htmlText(videoUrl(WiFi.localIP().toString())) + "</code></p>";
      if (localOnly) body += "<p>Conecte o Visão à mesma rede para abrir o vídeo.</p>";
      else body += "<p>ID da câmera: <code>" + htmlText(ESP32_CAMERA_ID) +
                   "</code></p><p>Copie o link de vídeo na área /espadmin do MapaZero Cam.</p>";
      body += "<p>O portal fecha automaticamente após a configuração.</p>";
      portal.sendHeader("Cache-Control", "no-store");
      portal.send(200, "text/html; charset=utf-8", body);
      return;
    }
    if (attempting) {
      body += "<meta http-equiv=refresh content=1><p>Tentando conectar ao Wi-Fi. Aguarde até 30 segundos.</p>";
      portal.sendHeader("Cache-Control", "no-store");
      portal.send(200, "text/html; charset=utf-8", body);
      return;
    }
    if (portalError) body += "<p>" + String(portalError) + "</p>";
    body += "<form method=post action=/wifi><input type=hidden name=token value='" + portalToken + "'>"
            "<p><label>Nome da rede <input name=ssid required maxlength=32></label></p>"
            "<p><label>Senha <input name=password type=password maxlength=64></label></p>"
            "<p><label>Resolucao maxima <select name=resolution>";
    for (unsigned i = 0; i < sizeof(resolutions) / sizeof(resolutions[0]); ++i) {
      body += "<option value='" + String(i) + "'";
      if (resolutions[i] == requestedResolution) body += " selected";
      body += ">" + String(resolutionNames[i]) + "</option>";
    }
    body += "</select></label></p><p><label>Modo de transmissão <select name=mode>";
    body += !localOnly && !cloudHttps ? "<option value=udp selected>" : "<option value=udp>";
    body += "MapaZero Cam (UDP)</option>";
    body += !localOnly && cloudHttps ? "<option value=https selected>" : "<option value=https>";
    body += "MapaZero Cam (HTTPS)</option>";
    body += localOnly ? "<option value=local selected>" : "<option value=local>";
    body += "Somente local (UDP)</option></select></label></p><button>Salvar e conectar</button></form>";
  } else {
    portal.send(403, "text/plain", "Conecte ao AP da placa para configurar.");
    return;
  }
  portal.sendHeader("Cache-Control", "no-store");
  portal.send(200, "text/html; charset=utf-8", body);
}

static void savePortalSettings() {
  if (!portalActive || portal.client().localIP() != WiFi.softAPIP() || portal.arg("token") != portalToken) {
    portal.send(403, "text/plain", "Conecte ao AP da placa para configurar.");
    return;
  }
  if (attempting) {
    portal.send(409, "text/plain", "Aguarde o fim da tentativa de conexao.");
    return;
  }
  const String ssid = portal.arg("ssid");
  const String password = portal.arg("password");
  const String index = portal.arg("resolution");
  const String mode = portal.arg("mode");
  const String settings = ssid + '\n' + password;
  if (!validWifi(settings) || ssid.indexOf('\n') >= 0 ||
      index.length() != 1 || index[0] < '0' || index[0] > '6' ||
      (mode != "local" && mode != "udp" && mode != "https")) {
    portal.send(400, "text/plain", "Configuracao invalida.");
    return;
  }
  candidateResolution = resolutions[index[0] - '0'];
  candidateLocalOnly = mode == "local";
  candidateCloudHttps = mode == "https";
  portal.send(200, "text/html; charset=utf-8",
              "<meta charset=utf-8><meta http-equiv=refresh content='1;url=/'>"
              "<p>Conectando ao Wi-Fi. Aguarde a confirmação.</p>");
  connectWifi(settings, true);
}

static void finishWifiFailure(const char* message) {
  attempting = false;
  saveCandidate = false;
  candidateWifi = "";
  portalError = message;
  WiFi.disconnect();
  WiFi.mode(portalActive ? WIFI_AP : WIFI_OFF);
}

static void serviceNetwork() {
  if (attempting) {
    if (WiFi.status() == WL_CONNECTED) {
      if (saveCandidate) {
        Preferences preferences;
        if (!preferences.begin(settingsNamespace) ||
            !preferences.putUChar("resolution", candidateResolution) ||
            !preferences.putBool("local-only", candidateLocalOnly) ||
            !preferences.putBool("cloud-https", candidateCloudHttps) ||
            !preferences.putString("wifi", candidateWifi)) {
          Serial.println("Wi-Fi sem conexao persistente: falha ao gravar configuracao.");
          finishWifiFailure("Falha ao salvar a configuração. Tente novamente.");
          return;
        }
        preferences.end();
        if (requestedResolution != candidateResolution && cameraReady) {
          esp_camera_deinit();
          cameraReady = false;
          lastCameraAttempt = millis() - 10000;
        }
        requestedResolution = candidateResolution;
        localOnly = candidateLocalOnly;
        cloudHttps = candidateCloudHttps;
      }
      candidateWifi = "";
      saveCandidate = false;
      attempting = false;
      connected = true;
      if (portalActive) portalConfirmedAt = millis();
      Serial.printf("Wi-Fi conectado; IP %s\n", WiFi.localIP().toString().c_str());
      flashConnected();
    } else if (millis() - wifiAttemptStarted >= wifiTimeoutMs) {
      Serial.printf("Wi-Fi sem conexao (status %d). Confira rede de 2,4 GHz e senha.\n", WiFi.status());
      finishWifiFailure("Conexão falhou. Confira o SSID e a senha da rede de 2,4 GHz.");
    }
    return;
  }
  if (connected && WiFi.status() != WL_CONNECTED) {
    connected = false;
    stopVideo();
    portalConfirmedAt = 0;
    portalError = "Wi-Fi desconectado. Confira a rede e tente novamente.";
    WiFi.mode(portalActive ? WIFI_AP : WIFI_OFF);
    Serial.println("Wi-Fi desconectado. Aperte RST para conectar novamente.");
  }
  if (connected && portalActive && millis() - portalConfirmedAt >= confirmationMs) {
    portal.stop();
    WiFi.softAPdisconnect(true);
    portalActive = false;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_GPIO_NUM, LOW);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  bool openPortal = false;
  String savedWifi;
  Preferences preferences;
  if (preferences.begin(settingsNamespace)) {
    openPortal = resetRequested(preferences);
    if (!openPortal) {
      savedWifi = preferences.getString("wifi", "");
      localOnly = preferences.getBool("local-only", false);
      cloudHttps = preferences.getBool("cloud-https", ESP32_USE_HTTPS);
      const auto storedResolution = preferences.getUChar("resolution", FRAMESIZE_QXGA);
      for (const auto resolution : resolutions) {
        if (storedResolution == resolution) requestedResolution = resolution;
      }
    }
    preferences.end();
  }
  lastCameraAttempt = millis() - 10000;
  if (openPortal) {
    portal.on("/", HTTP_GET, handlePage);
    portal.on("/wifi", HTTP_POST, savePortalSettings);
    startPortal();
  } else if (!connectWifi(savedWifi, false)) {
    Serial.println("Sem Wi-Fi salvo. Aperte RST 3 vezes rapidamente para abrir o AP.");
  }
  Serial.println("Configure o Wi-Fi no portal da placa");
}

void loop() {
  if (portalActive) portal.handleClient();
  if (Serial.available() && Serial.readStringUntil('\n') == "CONFIGURAR_WIFI") {
    const String ssid = Serial.readStringUntil('\n');
    const String password = Serial.readStringUntil('\n');
    candidateResolution = requestedResolution;
    candidateLocalOnly = localOnly;
    candidateCloudHttps = cloudHttps;
    if (!connectWifi(ssid + '\n' + password, true)) Serial.println("Configuracao Wi-Fi invalida");
  }
  serviceNetwork();
  serviceFlash();
  if (!connected) {
    delay(10);
    return;
  }
  if (reportedIp != WiFi.localIP()) {
    reportedIp = WiFi.localIP();
    if (!localOnly) configTime(0, 0, "pool.ntp.org", "time.google.com");
    Serial.printf("Destino: %s\n", videoUrl(reportedIp.toString()).c_str());
  }
  if (!senderReady) {
    if (static_cast<int32_t>(millis() - nextSenderAttempt) >= 0) {
      senderReady = startSender();
      nextSenderAttempt = millis() + 2000;
    }
    if (!senderReady) {
      delay(100);
      return;
    }
  }
  if (localOnly) {
    updateLocalReceiver();
    if (!localReceiver.sin_port) {
      delay(10);
      return;
    }
  }
  if (cameraReady) {
    sendFrame();
  } else {
    if (millis() - lastCameraAttempt >= 10000) {
      Serial.setDebugOutput(true);
      cameraReady = startCamera();
      Serial.setDebugOutput(false);
      lastCameraAttempt = millis();
    }
    delay(10);
  }
}
