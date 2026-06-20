/************************************************************
 * MEGA 2560 + W5500 + RS485 (FINAL - SCENE FIXED)
 ************************************************************/
#include <arduino.h>
#include <SPI.h>
#include <Ethernet2.h>

/* ===== FIRMWARE VERSION ===== */
#define FW_VERSION "1.3.0"

/* ================= CONFIG ================= */
#define TCP_PORT    9000
#define MAX_CLIENTS 4

#define RS485   Serial1
#define DE_PIN  22

#define MIRROR_BLOCK_TIME 300
#define CLIENT_TIMEOUT 10000

#define SCENE_PIN 30
#define DEBOUNCE_MS 50

// v1.1.0: timeout chờ phản hồi RS485 (trần an toàn, thường thoát sớm hơn nhiều)
#define RS485_READ_TIMEOUT_MS 60
// v1.1.0: giãn nhịp đọc I/O mirror để loop() rảnh xử lý lệnh TCP nhanh hơn
#define IO_POLL_INTERVAL_MS 120

//#define DEFAULT_LIGHT_MASK ((1 << 1) | (1 << 6))
#define DEFAULT_LIGHT_MASK (1 << 11)

/* ================= NETWORK ================= */
byte mac[] = {0xDE,0xAD,0xBE,0xEF,0x55,0x00};
IPAddress ip(192,168,1,178);
IPAddress gateway(192,168,1,1);
IPAddress subnet(255,255,255,0);

EthernetServer tcpServer(TCP_PORT);
EthernetClient clients[MAX_CLIENTS];

enum ClientType { CLIENT_NONE, CLIENT_TELNET, CLIENT_ESP };
ClientType clientType[MAX_CLIENTS];
unsigned long lastClientActivity[MAX_CLIENTS] = {0};

/* ================= RS485 ================= */
const byte relayToggle[16][8] = {
  {0x01,0x06,0x00,0x01,0x03,0x00,0xD8,0xFA},
  {0x01,0x06,0x00,0x02,0x03,0x00,0x28,0xFA},
  {0x01,0x06,0x00,0x03,0x03,0x00,0x79,0x3A},
  {0x01,0x06,0x00,0x04,0x03,0x00,0xC8,0xFB},
  {0x01,0x06,0x00,0x05,0x03,0x00,0x99,0x3B},
  {0x01,0x06,0x00,0x06,0x03,0x00,0x69,0x3B},
  {0x01,0x06,0x00,0x07,0x03,0x00,0x38,0xFB},
  {0x01,0x06,0x00,0x08,0x03,0x00,0x08,0xF8},
  {0x01,0x06,0x00,0x09,0x03,0x00,0x59,0x38},
  {0x01,0x06,0x00,0x0A,0x03,0x00,0xA9,0x38},
  {0x01,0x06,0x00,0x0B,0x03,0x00,0xF8,0xF8},
  {0x01,0x06,0x00,0x0C,0x03,0x00,0x49,0x39},
  {0x01,0x06,0x00,0x0D,0x03,0x00,0x18,0xF9},
  {0x01,0x06,0x00,0x0E,0x03,0x00,0xE8,0xF9},
  {0x01,0x06,0x00,0x0F,0x03,0x00,0xB9,0x39},
  {0x01,0x06,0x00,0x10,0x03,0x00,0x88,0xFF}
};

const byte relayAllOff[8] = {0x01,0x06,0x00,0x00,0x08,0x00,0x8E,0x0A};

const byte readInputsCmd[8]  = {0x01,0x03,0x00,0xC0,0x00,0x01,0x84,0x36};
const byte readOutputsCmd[8] = {0x01,0x03,0x00,0x70,0x00,0x01,0x85,0xD1};

/* ================= STATE ================= */
uint16_t lastInputs  = 0;
uint16_t inSnapshot  = 0;
uint16_t outSnapshot = 0;

unsigned long mirrorBlockUntil = 0;
bool needSync = false;
unsigned long syncRequestTime = 0;

uint16_t prevInBits = 0;

/* ================= RS485 ================= */
void rs485Send(const byte *cmd) {
  digitalWrite(DE_PIN, HIGH);
  delayMicroseconds(4);
  RS485.write(cmd, 8);
  RS485.flush();
  delayMicroseconds(4);
  digitalWrite(DE_PIN, LOW);
}

// v1.1.0: chờ đủ byte phản hồi nhưng thoát ngay khi đã có đủ, thay vì
// luôn luôn delay() cố định 60ms dù phản hồi đến sớm hơn nhiều.
bool waitForBytes(int count, unsigned long timeoutMs) {
  unsigned long t0 = millis();
  while ((int)RS485.available() < count) {
    if (millis() - t0 >= timeoutMs) return false;
  }
  return true;
}

bool readIO(uint16_t &inBits, uint16_t &outBits) {
  byte resp[8];

  while (RS485.available()) RS485.read();

  rs485Send(readInputsCmd);
  if (!waitForBytes(7, RS485_READ_TIMEOUT_MS)) return false;
  RS485.readBytes(resp, 7);
  inBits = (resp[3] << 8) | resp[4];

  while (RS485.available()) RS485.read();

  rs485Send(readOutputsCmd);
  if (!waitForBytes(7, RS485_READ_TIMEOUT_MS)) return false;
  RS485.readBytes(resp, 7);
  outBits = (resp[3] << 8) | resp[4];

  return true;
}

/* ================= SYNC ================= */
void requestSync() {
  needSync = true;
  syncRequestTime = millis();
}

/* ================= STATUS ================= */
void buildStatus(char *buf) {
  int p = 0;
  for (int i = 0; i < 16; i++) {
    p += sprintf(buf + p, "L%d=%d%s",
      i + 1,
      (outSnapshot >> i) & 1,
      (i == 15 ? "" : ","));
  }
}

/* ================= MIRROR ================= */
void mirrorInputs(uint16_t newInputs) {
  for (int i = 0; i < 16; i++) {
    if (bitRead(newInputs, i) != bitRead(lastInputs, i)) {
      rs485Send(relayToggle[i]);
      bool current = bitRead(outSnapshot, i);
      bitWrite(outSnapshot, i, !current);
      delay(120);
    }
  }
}

/* ================= EXEC COMMAND ================= */
void execCommand(const char *cmd) {

  if (strncmp(cmd, "set /relay/", 11) == 0) {

    int ch = atoi(cmd + 11);
    if (ch < 1 || ch > 16) return;

    if (strstr(cmd, "/toggle")) {
      rs485Send(relayToggle[ch - 1]);
      bitWrite(outSnapshot, ch - 1, !bitRead(outSnapshot, ch - 1));
      mirrorBlockUntil = millis() + MIRROR_BLOCK_TIME;
      requestSync();
      return;
    }

    if (strstr(cmd, "/state")) {
      bool isOn = strstr(cmd, "true");
      bool current = bitRead(outSnapshot, ch - 1);

      if (current != isOn) {
        rs485Send(relayToggle[ch - 1]);
        bitWrite(outSnapshot, ch - 1, isOn);
      }

      mirrorBlockUntil = millis() + MIRROR_BLOCK_TIME;
      requestSync();
      return;
    }
  }

  if (strcmp(cmd, "set /system/all off") == 0) {
    rs485Send(relayAllOff);
    // v1.3.0: cập nhật lạc quan outSnapshot=0 ngay lập tức, khớp với cách
    // nhánh /relay/.../state đã làm — trước đây thiếu bước này khiến phản
    // hồi trạng thái gửi ngay về ESP32 vẫn còn các bit ON cũ (đèn đã tắt
    // thật nhưng UI vẫn hiện xanh), vì bản sync đúng 150ms sau đó chỉ sửa
    // outSnapshot nội bộ mà không có gì chủ động đẩy lại cho client TCP.
    outSnapshot = 0;
    mirrorBlockUntil = millis() + MIRROR_BLOCK_TIME;
    requestSync();
    return;
  }

  if (strcmp(cmd, "get /relay/all") == 0) {
    requestSync();
    return;
  }
}

/* ================= SCENE (FIXED) ================= */
bool anyLightOn() {
  return (outSnapshot & 0x0FFF) != 0;
}

void restoreLights(uint16_t target) {
  for (int i = 0; i < 12; i++) {
    bool want = bitRead(target, i);
    bool curr = bitRead(outSnapshot, i);
    if (want != curr) {
      rs485Send(relayToggle[i]);
      delay(120);
    }
  }
}

void handleSceneSwitch() {

  static bool lastStable = HIGH;
  static bool lastRead   = HIGH;
  static unsigned long lastChange = 0;

  bool now = digitalRead(SCENE_PIN);

  if (now != lastRead) {
    lastChange = millis();
    lastRead = now;
  }

  if ((millis() - lastChange) > DEBOUNCE_MS) {

    if (lastStable == HIGH && now == LOW) {

      uint16_t currentLights = outSnapshot & 0x0FFF;

      if (currentLights != 0) {
        rs485Send(relayAllOff);
      } else {
        restoreLights(DEFAULT_LIGHT_MASK);
      }

      mirrorBlockUntil = millis() + MIRROR_BLOCK_TIME;
      requestSync();
    }

    lastStable = now;
  }
}

/* ================= TCP ================= */
void acceptClients() {
  EthernetClient nc = tcpServer.available();
  if (!nc) return;

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i] || !clients[i].connected()) {
      clients[i] = nc;
      clientType[i] = CLIENT_TELNET;
      lastClientActivity[i] = millis();
      return;
    }
  }
  nc.stop();
}

void handleTCP() {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    EthernetClient &cl = clients[i];
    if (cl && !cl.connected()) {
      cl.stop();
      clientType[i] = CLIENT_NONE;
      continue;
    }
    if (!cl || !cl.available()) continue;

    String line = cl.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    execCommand(line.c_str());

    char buf[160];
    buildStatus(buf);
    cl.println(buf);
  }
}

// v1.2.0: kiểm tra có client nào đang chờ lệnh xử lý không, dùng để
// nhường ưu tiên cho xử lý lệnh TCP thay vì vòng polling mirror I/O —
// tránh cộng hưởng giữa nhịp bấm nút dồn dập và chu kỳ polling khiến
// lệnh bị kẹt phía sau vòng đọc RS485.
bool anyClientPending() {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i] && clients[i].connected() && clients[i].available()) {
      return true;
    }
  }
  return false;
}

/* ================= SETUP ================= */
void setup() {

  pinMode(DE_PIN, OUTPUT);
  digitalWrite(DE_PIN, LOW);

  pinMode(SCENE_PIN, INPUT_PULLUP);
  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH);

  Serial.begin(115200);
  delay(1200);

  Serial.print("MEGA Firmware v");
  Serial.println(FW_VERSION);

  Ethernet.begin(mac, ip, gateway, gateway, subnet);
  tcpServer.begin();

  RS485.begin(9600);

  readIO(inSnapshot, outSnapshot);
  lastInputs = inSnapshot;

  Serial.println("READY - FINAL");
}

/* ================= LOOP ================= */
void loop() {

  handleSceneSwitch();   // SCENE OK

  acceptClients();
  handleTCP();

  static unsigned long lastIOPoll = 0;
  uint16_t inBits, outBits;

  // v1.1.0: chỉ poll mirror mỗi IO_POLL_INTERVAL_MS thay vì mọi vòng loop(),
  // để dành chu kỳ CPU cho acceptClients()/handleTCP() xử lý lệnh relay
  // từ mạng nhanh hơn, không bị kẹt phía sau vòng polling mirror.
  // v1.2.0: bỏ qua lượt poll này nếu đang có lệnh TCP khác chờ xử lý —
  // tránh trường hợp bấm nút dồn dập rơi đúng nhịp polling I/O (~120ms)
  // khiến lệnh bị delay tới gần 120ms mỗi lần do loop() còn đang bận
  // trong readIO(). Tính năng mirror công tắc vật lý chỉ bị lùi lại nhẹ,
  // chấp nhận được vì tốc độ thao tác công tắc tay không nhanh bằng bấm
  // nút liên tục trên UI.
  if (!needSync && millis() > mirrorBlockUntil &&
      millis() - lastIOPoll >= IO_POLL_INTERVAL_MS &&
      !anyClientPending()) {

    lastIOPoll = millis();

    if (readIO(inBits, outBits)) {

      if (millis() > mirrorBlockUntil) {
        inSnapshot = inBits;
        outSnapshot = outBits;
      }

      if (millis() > mirrorBlockUntil) {
        if (inBits == prevInBits && inBits != lastInputs) {
          mirrorInputs(inBits);
          lastInputs = inBits;
        }
        prevInBits = inBits;
      }
    }
  }

  if (needSync &&
      millis() - syncRequestTime > 150 &&
      millis() > mirrorBlockUntil) {

    uint16_t i, o;
    if (readIO(i, o)) {
      lastInputs = inSnapshot = i;
      outSnapshot = o;
    }
    needSync = false;
  }
}
