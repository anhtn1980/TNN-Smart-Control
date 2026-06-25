#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>

/* ===== FIRMWARE VERSION ===== */
#define FW_VERSION "2.8.0"

/* ===== W5500 PIN CONFIG ===== */
#define W5500_CS 5
#define W5500_RST 4
#define SPI_SCK 18
#define SPI_MISO 19
#define SPI_MOSI 23

/* ===== ETHERNET CONFIG ===== */
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
IPAddress ip(192, 168, 1, 180);
IPAddress dns(8, 8, 8, 8);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

/* ===== MEGA CONFIG ===== */
const char* megaIP = "192.168.1.178";
const int megaPort = 9000;
// v2.0.0: timeout chờ MEGA phản hồi — MEGA có thể delay tối đa ~120ms (readIO x2)
#define MEGA_TIMEOUT_MS 300

/* ===== AMX CONFIG ===== */
const char* amxIoIP    = "192.168.1.7";    // CE-IO4
const char* amxRelayIP = "192.168.1.204";  // CE-REL8
const int   amxPort    = 44197;
#define AMX_TRANSACT_TIMEOUT_MS 150

/* ===== LOGO! MODBUS TCP CONFIG ===== */
const char* logoIP = "192.168.1.6";
const int   logoPort = 504;
#define LOGO_UNIT_ID        1
#define LOGO_M_ADDR         4
#define LOGO_FB_ADDR        0
#define LOGO_PULSE_MS       500
#define LOGO_COOLDOWN_MS    1500
#define LOGO_MODBUS_TIMEOUT_MS 200

/* ===== BASIC AUTH ===== */
// Chỉ cần password — trình duyệt hiện 1 ô "Password", không hỏi username
// Đổi AUTH_PASS và AUTH_B64 (= Base64 của ":password") trước khi nạp firmware
// Tính lại: echo -n ":mat_khau_moi" | base64
#define AUTH_PASS "tnn@2026"
static const char AUTH_B64[] = "OnRubkAyMDI2";  // Base64(":tnn@2026")

/* ===== SERVER ===== */
EthernetServer server(80);
// v2.0.0: KHÔNG có persistent megaClient — dùng connect-per-transaction
// Đây là giải pháp dứt điểm cho vấn đề "RECONNECTED OK" loop dưới tải cao:
// persistent connection bị drop bởi W5500 khi nhiều HTTP socket mở/đóng đồng thời.
// Mỗi lệnh relay: connect → gửi → đọc response → disconnect. Không bao giờ stale.
String globalStatus = "";
uint8_t acSnapshot = 0;

/* ===== AMX STATE ===== */
uint8_t amxRelaySnapshot = 0;  // bits 0-3: relay 1-4
uint8_t amxInputSnapshot = 0;  // bits 0-3: IO 1-4

// Toggle: mỗi lần công tắc tường đổi trạng thái (bất kể chiều) → toggle relay tương ứng.
// Delay 500ms nhường Kramer phản hồi trước — nếu Kramer đã toggle thì relay đã đúng,
// ESP32 toggle thêm lần nữa sẽ sai → đọc relay thật trước khi toggle để tránh.
// Sau delay, đọc relay thật: nếu trạng thái relay chưa thay đổi so với trước khi IO đổi
// → Kramer chưa phản hồi → ESP32 toggle. Nếu relay đã thay đổi → Kramer đã xử lý → bỏ qua.
bool          amxNeedsReconcile = false;
unsigned long amxReconcileAfter = 0;
uint8_t       amxPendingToggle  = 0;
uint8_t       amxRelayBeforeIO  = 0;
#define AMX_RECONCILE_DELAY_MS 300

// Persistent connection tới CE-IO4 — tránh mở/đóng socket liên tục làm cạn W5500
EthernetClient amxIoClient;
String         amxIoRxBuf      = "";
unsigned long  amxIoLastOk     = 0;      // lần cuối nhận được response hợp lệ
#define AMX_IO_RECONNECT_MS    5000      // thử kết nối lại nếu mất 5 giây
#define AMX_IO_POLL_INTERVAL_MS 600

/* ===== NTP + SCHEDULER ===== */
// NTP server — dùng IP router nội bộ nếu router có NTP relay, hoặc pool.ntp.org
static const char NTP_SERVER[] = "192.168.1.1";
#define NTP_PORT          123
#define TZ_OFFSET_SEC     25200   // UTC+7 (Việt Nam)
#define NTP_RESYNC_MS     21600000UL  // re-sync mỗi 6 tiếng
#define NTP_RETRY_MS      30000       // retry nếu chưa sync được

// Thời điểm tắt thiết bị: 18:00 mỗi ngày
// Thêm mốc nếu cần: {{18,0},{22,30},...}
struct ScheduleEntry { uint8_t hour; uint8_t minute; };
static const ScheduleEntry SCHEDULE[] = { {18, 0} };
#define SCHEDULE_COUNT    (sizeof(SCHEDULE)/sizeof(SCHEDULE[0]))
#define SCHEDULE_WINDOW_S 300   // chạy nếu boot lên trong vòng 5 phút sau mốc

EthernetUDP ntpUdp;
bool    ntpSynced       = false;
unsigned long ntpEpoch  = 0;   // Unix timestamp lúc sync
unsigned long ntpMillis = 0;   // millis() lúc sync
unsigned long lastNtpAttempt = 0;

// Theo dõi mốc nào đã kích hoạt hôm nay (bit mask theo index SCHEDULE[])
uint8_t schedFiredToday = 0;
int     schedLastDay    = -1;

// Lấy Unix timestamp hiện tại (UTC+7)
unsigned long nowEpochLocal() {
  if (!ntpSynced) return 0;
  return ntpEpoch + (millis() - ntpMillis) / 1000;
}

// Tách giờ/phút/ngày từ epoch local
void epochToHMS(unsigned long ep, uint8_t &h, uint8_t &m, uint8_t &s, int &day) {
  ep %= 86400UL * 36525UL;   // tránh overflow
  day = (int)(ep / 86400);
  unsigned long tod = ep % 86400;
  h = tod / 3600;
  m = (tod % 3600) / 60;
  s = tod % 60;
}

// Gửi NTP request và đọc response — blocking tối đa 1.5s
bool ntpSync() {
  uint8_t buf[48];
  memset(buf, 0, 48);
  buf[0] = 0b11100011;  // LI=3, VN=4, Mode=3 (client)
  buf[1] = 0; buf[2] = 6; buf[3] = 0xEC;
  buf[12] = 49; buf[13] = 0x4E; buf[14] = 49; buf[15] = 52;

  ntpUdp.begin(NTP_PORT);
  ntpUdp.beginPacket(NTP_SERVER, NTP_PORT);
  ntpUdp.write(buf, 48);
  ntpUdp.endPacket();

  unsigned long t0 = millis();
  while (millis() - t0 < 1500) {
    if (ntpUdp.parsePacket() >= 48) {
      ntpUdp.read(buf, 48);
      ntpUdp.stop();
      unsigned long hi = (unsigned long)buf[40] << 24 | (unsigned long)buf[41] << 16 |
                         (unsigned long)buf[42] << 8  | buf[43];
      if (hi < 2208988800UL) { return false; }  // sanity check
      ntpEpoch  = hi - 2208988800UL + TZ_OFFSET_SEC;
      ntpMillis = millis();
      ntpSynced = true;
      Serial.print("NTP synced: epoch="); Serial.println(ntpEpoch);
      return true;
    }
    yield();
  }
  ntpUdp.stop();
  return false;
}

// Kiểm tra và chạy scheduler — gọi trong loop() mỗi giây
void runScheduler() {
  if (!ntpSynced) return;
  unsigned long ep = nowEpochLocal();
  uint8_t h, m, s; int day;
  epochToHMS(ep, h, m, s, day);

  // Reset fired flags khi sang ngày mới
  if (day != schedLastDay) {
    schedFiredToday = 0;
    schedLastDay = day;
  }

  for (int i = 0; i < SCHEDULE_COUNT; i++) {
    if (bitRead(schedFiredToday, i)) continue;
    int schedSec = SCHEDULE[i].hour * 3600 + SCHEDULE[i].minute * 60;
    int nowSec   = h * 3600 + m * 60 + s;
    int diff = nowSec - schedSec;
    // Kích hoạt nếu đúng giờ hoặc boot lên trong vòng SCHEDULE_WINDOW_S giây sau
    if (diff >= 0 && diff <= SCHEDULE_WINDOW_S) {
      bitSet(schedFiredToday, i);
      Serial.printf("Scheduler fired %02d:%02d — tắt tất cả thiết bị\n",
                    SCHEDULE[i].hour, SCHEDULE[i].minute);
      // Tắt 16 relay MEGA (nếu có thiết bị bật)
      if (globalStatus.indexOf("=1") >= 0)
        megaTransact("set /system/all off");
      // Tắt 4 relay AMX CE-REL8
      for (int ch = 1; ch <= 4; ch++) {
        if (bitRead(amxRelaySnapshot, ch - 1))
          amxSetRelay(ch, false);
      }
      // Tắt 4 điều hòa LOGO! (nếu đang bật)
      for (int ch = 0; ch < 4; ch++) {
        if (bitRead(acSnapshot, ch))
          logoSendPulse(ch);
      }
    }
  }
}

/* ===== LOGO! PULSE STATE MACHINE ===== */
struct LogoPulse { bool active; int channel; unsigned long startMs; };
LogoPulse logoPulse = {false, -1, 0};
unsigned long acCooldownMs[4] = {0, 0, 0, 0};


String normalizeStatusLine(const String &raw) {
  int start = raw.lastIndexOf("L1=");
  if (start < 0) return "";
  String part = raw.substring(start);
  part.replace("\r", ""); part.replace("\n", "");
  int end = part.lastIndexOf("L16=");
  if (end < 0) return part;
  int commaAfter = part.indexOf(',', end);
  if (commaAfter > 0) return part.substring(0, commaAfter);
  return part;
}

/* ===== RESET W5500 ===== */
void resetW5500() {
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, HIGH); delay(100);
  digitalWrite(W5500_RST, LOW);  delay(200);
  digitalWrite(W5500_RST, HIGH); delay(200);
}

/* ===== MEGA TRANSACT (v2.0.0 — connect-per-transaction) ===== */
// Gửi 1 lệnh tới MEGA, đọc response, cập nhật globalStatus, đóng kết nối.
// Pattern giống LOGO! logoTransact() — không giữ persistent connection → không bao giờ stale/drop.
// Blocking tối đa MEGA_TIMEOUT_MS (300ms) — chấp nhận được vì MEGA thường phản hồi <120ms.
bool megaTransact(const String &cmd) {
  EthernetClient c;
  if (!c.connect(megaIP, megaPort)) {
    Serial.println("MEGA connect fail");
    return false;
  }
  Serial.println("TX: " + cmd);
  c.print(cmd + "\r\n");

  // Đọc response line từ MEGA
  unsigned long t0 = millis();
  String line = "";
  while (millis() - t0 < MEGA_TIMEOUT_MS) {
    while (c.available()) {
      char ch = (char)c.read();
      if (ch == '\n') {
        line.trim();
        String normalized = normalizeStatusLine(line);
        if (normalized.length() > 0) globalStatus = normalized;
        c.stop();
        return true;
      } else if (ch != '\r') {
        line += ch;
      }
    }
    yield();
  }
  // Timeout — cập nhật status nếu có gì
  line.trim();
  String normalized = normalizeStatusLine(line);
  if (normalized.length() > 0) globalStatus = normalized;
  c.stop();
  return false;
}

/* ===== MODBUS TCP — LOGO! (connect-per-transaction) ===== */
bool logoTransact(byte* reqFrame, int reqLen, byte* respBuf, int expectedLen) {
  EthernetClient c;
  if (!c.connect(logoIP, logoPort)) return false;
  c.write(reqFrame, reqLen);
  unsigned long t0 = millis();
  while (c.available() < expectedLen && millis() - t0 < LOGO_MODBUS_TIMEOUT_MS) { yield(); }
  bool ok = (c.available() >= expectedLen);
  if (ok && respBuf) c.readBytes(respBuf, expectedLen);
  while (c.available()) c.read();
  c.stop();
  return ok;
}

void logoFC5(uint16_t addr, bool val) {
  byte frame[12] = {0x00,0x01,0x00,0x00,0x00,0x06,LOGO_UNIT_ID,0x05,
    (byte)(addr>>8),(byte)(addr&0xFF),(byte)(val?0xFF:0x00),0x00};
  logoTransact(frame, 12, nullptr, 12);
}

void logoSendPulse(int ch) {
  if (logoPulse.active) return;
  if (millis() - acCooldownMs[ch] < LOGO_COOLDOWN_MS) return;
  logoFC5(LOGO_M_ADDR + ch, true);
  logoPulse = {true, ch, millis()};
  acCooldownMs[ch] = millis();
}

bool readACFeedback() {
  // FC1 Read Coils — đọc V0.0-V0.3 (feedback trạng thái thật từ LOGO!)
  byte req[12] = {0x00,0x02,0x00,0x00,0x00,0x06,LOGO_UNIT_ID,0x01,
    (byte)(LOGO_FB_ADDR>>8),(byte)(LOGO_FB_ADDR&0xFF),0x00,0x04};
  byte resp[10];
  if (!logoTransact(req, 12, resp, 10)) return false;
  // resp[9] = coil status byte: bit0=ĐH1, bit1=ĐH2, bit2=ĐH3, bit3=ĐH4
  acSnapshot = resp[9] & 0x0F;
  return true;
}

/* ===== AMX — connect-per-transaction ===== */
// Gửi nhiều lệnh trong 1 connection, đọc đến maxLines dòng response.
// Mỗi dòng được trả về qua callback onLine(String&).
bool amxMultiQuery(const char* deviceIP, const String* cmds, int numCmds,
                   void (*onLine)(const String&), int maxLines) {
  EthernetClient c;
  if (!c.connect(deviceIP, amxPort)) return false;
  for (int i = 0; i < numCmds; i++) { c.print(cmds[i] + "\n"); }
  unsigned long t0 = millis(); int got = 0; String line = "";
  while (got < maxLines && millis() - t0 < AMX_TRANSACT_TIMEOUT_MS) {
    while (c.available()) {
      char ch = c.read();
      if (ch == '\n') { if (line.length()) { Serial.print("AMX RAW: ["); Serial.print(line); Serial.println("]"); onLine(line); got++; } line = ""; }
      else if (ch != '\r') line += ch;
    }
    yield();
  }
  c.stop();
  return got > 0;
}

void _parseAmxIoLine(const String& line) {
  // "update /io/N/digitalInput true|false"  (per CE-IO4 manual rev05 p.44)
  if (!line.startsWith("update /io/")) return;
  int n = line.charAt(11) - '1';  // '1'→0 .. '4'→3
  if (n < 0 || n > 3) return;
  bool newVal = line.endsWith("true");
  Serial.print("AMX IO RX: ["); Serial.print(line); Serial.println("]");
  if (bitRead(amxInputSnapshot, n) != newVal) {
    bitWrite(amxInputSnapshot, n, newVal);
    bitWrite(amxPendingToggle, n, true);   // đánh dấu kênh này cần toggle
    amxRelayBeforeIO = amxRelaySnapshot;   // ghi nhớ trạng thái relay lúc này
    amxNeedsReconcile = true;
    amxReconcileAfter = millis() + AMX_RECONCILE_DELAY_MS;
  }
}

void _parseAmxRelayLine(const String& line) {
  // "update /relay/N/state true|false"
  if (!line.startsWith("update /relay/")) return;
  int n = line.charAt(14) - '1'; // '1'→0 .. '4'→3
  if (n < 0 || n > 3) return;
  bitWrite(amxRelaySnapshot, n, line.endsWith("true"));
}

// Gửi 4 lệnh get trên persistent connection — không mở/đóng socket.
// Non-blocking: gửi lệnh rồi trả về ngay, response đọc ở amxIoPoll().
void amxIoSendGet() {
  if (!amxIoClient.connected()) return;
  for (int i = 1; i <= 4; i++)
    amxIoClient.print(String("get /io/") + i + "/digitalInput\n");
}

// Đọc response từ CE-IO4 — non-blocking, gọi mỗi vòng loop().
void amxIoPoll() {
  while (amxIoClient.available()) {
    char c = amxIoClient.read();
    if (c == '\n') {
      amxIoRxBuf.trim();
      if (amxIoRxBuf.length()) {
        Serial.print("AMX RAW: ["); Serial.print(amxIoRxBuf); Serial.println("]");
        _parseAmxIoLine(amxIoRxBuf);
        amxIoLastOk = millis();
      }
      amxIoRxBuf = "";
    } else if (c != '\r') {
      amxIoRxBuf += c;
    }
  }
}

bool amxReadInputs() {
  amxIoSendGet();
  return amxIoClient.connected();
}

bool amxReadRelays() {
  String cmds[4];
  for (int i = 0; i < 4; i++) cmds[i] = "Subscribe /relay/" + String(i+1) + "/state";
  return amxMultiQuery(amxRelayIP, cmds, 4, _parseAmxRelayLine, 4);
}

void amxSetRelay(int ch, bool val) {
  // ch = 1..4
  String cmd = "set /relay/" + String(ch) + "/state " + (val ? "true" : "false");
  String cmds[1] = {cmd};
  amxMultiQuery(amxRelayIP, cmds, 1, _parseAmxRelayLine, 1);
  bitWrite(amxRelaySnapshot, ch-1, val);  // optimistic update
}

// Kết nối persistent tới CE-IO4, cấu hình inputMode, seed snapshot ban đầu.
bool amxIoConnect() {
  amxIoClient.stop();
  if (!amxIoClient.connect(amxIoIP, amxPort)) {
    Serial.println("AMX IO connect fail");
    return false;
  }
  // Set inputMode DIGITAL cho 4 port (có thể fail nếu firmware không hỗ trợ — ok)
  for (int i = 1; i <= 4; i++) {
    amxIoClient.print(String("set /io/") + i + "/mode INPUT\n");
    amxIoClient.print(String("set /io/") + i + "/inputMode DIGITAL\n");
  }
  delay(200);  // chờ CE-IO4 xử lý set commands
  while (amxIoClient.available()) amxIoClient.read();  // flush responses

  // Seed: đọc trạng thái hiện tại, KHÔNG trigger toggle
  for (int i = 1; i <= 4; i++)
    amxIoClient.print(String("get /io/") + i + "/digitalInput\n");
  unsigned long t0 = millis(); amxIoRxBuf = "";
  while (millis() - t0 < 300) {
    while (amxIoClient.available()) {
      char c = amxIoClient.read();
      if (c == '\n') {
        amxIoRxBuf.trim();
        if (amxIoRxBuf.startsWith("update /io/")) {
          int n = amxIoRxBuf.charAt(11) - '1';
          if (n >= 0 && n < 4) bitWrite(amxInputSnapshot, n, amxIoRxBuf.endsWith("true"));
        }
        amxIoRxBuf = "";
      } else if (c != '\r') amxIoRxBuf += c;
    }
  }
  amxIoRxBuf = "";
  amxNeedsReconcile = false;
  amxPendingToggle  = 0;
  amxIoLastOk = millis();
  Serial.println("AMX IO connected & seeded");
  return true;
}

// Toggle relay khi công tắc tường thay đổi — chạy sau AMX_RECONCILE_DELAY_MS.
// Đọc relay thật trước: nếu relay đã thay đổi so với lúc IO đổi → Kramer đã xử lý → bỏ qua.
// Nếu relay chưa thay đổi → Kramer chưa phản hồi → ESP32 toggle.
void amxReconcile() {
  amxReadRelays();  // cập nhật amxRelaySnapshot
  for (int i = 0; i < 4; i++) {
    if (!bitRead(amxPendingToggle, i)) continue;
    bitWrite(amxPendingToggle, i, false);
    bool before = bitRead(amxRelayBeforeIO, i);
    bool now    = bitRead(amxRelaySnapshot, i);
    if (now == before) {
      // Relay chưa thay đổi → Kramer chưa toggle → ESP32 toggle
      amxSetRelay(i + 1, !now);
    }
    // Nếu now != before → Kramer đã toggle rồi → không làm gì
  }
}

/* ===== BASIC AUTH HELPER ===== */
// Trả true nếu request chứa đúng Authorization header
bool checkAuth(const String& req) {
  int idx = req.indexOf("Authorization: Basic ");
  if (idx < 0) return false;
  int end = req.indexOf("\r\n", idx + 21);
  String token = (end > 0) ? req.substring(idx + 21, end) : req.substring(idx + 21);
  token.trim();
  return token == AUTH_B64;
}

void send401(EthernetClient& client) {
  client.println("HTTP/1.1 401 Unauthorized\r\n"
                 "WWW-Authenticate: Basic realm=\"TNN Smart Control\"\r\n"
                 "Content-Type: text/plain\r\n"
                 "Connection: close\r\n");
  client.println("401 Unauthorized");
  client.stop();
}

/* ===== HTTP HANDLER ===== */
void handleWebRequest(EthernetClient client) {
  String request = "";
  bool headerComplete = false;
  unsigned long t0 = millis();
  while (!headerComplete && millis() - t0 < 30) {
    while (client.available()) {
      char c = client.read();
      request += c;
      if (request.indexOf("\r\n\r\n") >= 0) { headerComplete = true; break; }
    }
    if (headerComplete || !client.connected()) break;
    yield();
  }
  if (!headerComplete) { client.stop(); return; }

  // ===== BASIC AUTH CHECK =====
  if (!checkAuth(request)) { send401(client); return; }

  // ===== AC TOGGLE =====
  if (request.indexOf("GET /ac/toggle?") >= 0) {
    int chPos = request.indexOf("ch=") + 3;
    int chEnd = request.indexOf(" ", chPos);
    int ch = request.substring(chPos, chEnd).toInt();
    bool sent = false;
    if (ch >= 0 && ch <= 3) {
      logoSendPulse(ch);
      sent = true;
    }
    client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n");
    client.print("{\"sent\":"); client.print(sent ? "true" : "false");
    client.print(",\"ch\":"); client.print(ch); client.println("}");
    client.stop(); return;
  }

  // ===== AC STATUS =====
  if (request.indexOf("GET /ac/status") >= 0) {
    readACFeedback();
    client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n");
    client.print("{\"ac\":[");
    for (int i = 0; i < 4; i++) {
      client.print((acSnapshot >> i) & 1);
      if (i < 3) client.print(",");
    }
    client.println("]}");
    client.stop(); return;
  }

  // ===== API SET =====
  if (request.indexOf("GET /set?") >= 0) {
    int rPos = request.indexOf("relay=") + 6;
    int vPos = request.indexOf("value=") + 6;
    int rEnd = request.indexOf("&", rPos);
    int vEnd = request.indexOf(" ", vPos);
    if (rEnd == -1) rEnd = request.indexOf(" ", rPos);
    String relayNum = request.substring(rPos, rEnd);
    String valStr   = request.substring(vPos, vEnd);
    bool isOn = (valStr == "true" || valStr == "1");
    megaTransact("set /relay/" + relayNum + "/state " + (isOn ? "true" : "false"));
    client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n");
    client.print("{\"status\":\"success\",\"relay\":"); client.print(relayNum);
    client.print(",\"value\":"); client.print(isOn ? "true" : "false"); client.println("}");
    client.stop(); return;
  }

  // ===== API STATUS JSON =====
  if (request.indexOf("GET /api/status") >= 0) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n");
    client.print("{\"raw\":\""); client.print(globalStatus); client.println("\"}");
    client.stop(); return;
  }

  /* ===== SPA — Single Page Application ===== */
  if (request.startsWith("GET / ") || request.startsWith("GET /mega") ||
      request.startsWith("GET /modbus") || request.startsWith("GET /amx ") ||
      request.startsWith("GET /kios")) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\n");
    // ── HEAD ──
    client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>TNN Smart Control</title>");
    client.println("<style>");
    client.println("*{box-sizing:border-box;margin:0;padding:0}");
    client.println("body{font-family:Arial,sans-serif;background:#111;color:#fff;display:flex;flex-direction:column;height:100vh;overflow:hidden}");
    // nav bar
    client.println(".nav{display:flex;background:#1a1a1a;border-bottom:1px solid #333;flex-shrink:0}");
    client.println(".nav-btn{flex:1;padding:11px 4px;font-size:12px;text-align:center;background:none;border:none;color:#888;cursor:pointer;border-bottom:2px solid transparent;transition:.15s}");
    client.println(".nav-btn.active{color:#fff;border-bottom-color:#4a9eff}");
    client.println(".nav-btn:hover{color:#ccc}");
    // pages
    client.println(".page{display:none;flex:1;overflow-y:auto;flex-direction:column}");
    client.println(".page.active{display:flex}");
    // shared top bar
    client.println(".top{display:flex;align-items:center;gap:8px;padding:8px 12px;background:#1a1a1a;border-bottom:1px solid #333;flex-shrink:0;flex-wrap:wrap}");
    client.println(".top h2{margin:0;font-size:15px;flex:1}");
    client.println(".info-btn{padding:5px 9px;border-radius:6px;background:#2a2a2a;color:#aaa;border:1px solid #444;cursor:pointer;font-size:12px}");
    client.println(".info-btn:hover{background:#333;color:#fff}");
    client.println(".info-panel{display:none;text-align:left;background:#181818;border-bottom:1px solid #333;padding:12px 14px;font-size:11.5px;line-height:1.7;color:#bbb;flex-shrink:0}");
    client.println(".info-panel h4{color:#fff;margin:0 0 4px;font-size:12px}");
    client.println(".info-panel code{color:#88ccff;background:#1a2a3a;padding:1px 4px;border-radius:3px}");
    client.println(".cfg{display:grid;grid-template-columns:auto 1fr;gap:3px 10px;margin-top:6px}");
    client.println(".cfg .k{color:#888}.cfg .v{color:#7dd3fc;font-family:monospace}");
    // mega
    client.println(".grid16{display:grid;grid-template-columns:repeat(auto-fit,minmax(95px,1fr));gap:8px;padding:10px}");
    client.println(".grid16 button{height:56px;font-size:11px;border-radius:9px;border:none;background:#333;color:#fff;cursor:pointer;transition:.1s}");
    client.println(".grid16 button:active,.grid16 button.pressing{transform:scale(.95);background:#666}");
    client.println(".grid16 button.on{background:#1a6a1a}");
    // ac
    client.println(".grid4{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:12px;padding:20px 14px}");
    client.println(".ac-btn{height:86px;border-radius:12px;border:2px solid #444;background:#222;color:#fff;font-size:13px;font-weight:bold;cursor:pointer;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:5px;transition:.15s}");
    client.println(".ac-btn .icon{font-size:24px}.ac-btn.on{background:#0a4a7a;border-color:#1a8fd1;color:#7dd3fc}");
    client.println(".ac-btn.cooling{opacity:.5;pointer-events:none}.ac-btn:active{transform:scale(.96)}");
    client.println(".dot{width:7px;height:7px;border-radius:50%;background:#555}.on .dot{background:#1a8fd1}");
    // amx
    client.println(".section{padding:10px 12px}.section-title{font-size:12px;color:#888;margin-bottom:8px;text-transform:uppercase;letter-spacing:.5px}");
    client.println(".relay-btn{width:100%;padding:14px;border-radius:10px;border:1px solid #444;background:#222;color:#fff;font-size:14px;cursor:pointer;display:flex;align-items:center;gap:10px;margin-bottom:8px;transition:.15s}");
    client.println(".relay-btn.on{background:#1a4a1a;border-color:#2a7a2a}.relay-btn .icon{font-size:20px}");
    client.println(".io-grid{display:flex;gap:12px;flex-wrap:wrap}.io-indicator{text-align:center;font-size:11px;color:#888}");
    client.println(".io-dot{width:14px;height:14px;border-radius:50%;background:#333;margin:4px auto 0}");
    client.println(".io-indicator.active .io-dot{background:#55aaff}");
    // kios
    client.println("#kios-page{padding:0}");
    client.println(".kios-bar{display:flex;gap:6px;padding:7px 10px;background:#111;border-bottom:1px solid #222;flex-shrink:0;flex-wrap:wrap}");
    client.println(".site-btn{padding:6px 14px;border-radius:7px;border:1px solid #335;background:#1a1a2a;color:#aad4ff;font-size:12px;font-weight:bold;cursor:pointer}");
    client.println(".site-btn.active{background:#225588;border-color:#4488cc;color:#fff}");
    client.println(".site-btn.add{background:#1a2a1a;border-color:#3a5a3a;color:#88cc88}");
    client.println(".url-bar{display:none;gap:6px;padding:6px 10px;background:#141414;border-bottom:1px solid #222;flex-shrink:0;align-items:center;flex-wrap:wrap}");
    client.println("#url-input{flex:1;min-width:160px;padding:5px 9px;border-radius:6px;border:1px solid #444;background:#222;color:#fff;font-size:12px}");
    client.println(".kbtn{padding:5px 10px;border-radius:6px;color:#fff;border:none;cursor:pointer;font-size:12px}");
    client.println(".kios-frame{flex:1;width:100%;border:none;background:#fff}");
    // status bar
    client.println(".sbar{font-size:10px;opacity:.4;padding:6px 12px;flex-shrink:0}");
    client.println("</style></head><body>");

    // ── NAV BAR ──
    client.println("<div class='nav'>");
    client.println("<button class='nav-btn' onclick='nav(0)'>🔌<br>Đèn</button>");
    client.println("<button class='nav-btn' onclick='nav(1)'>❄️<br>Điều hòa</button>");
    client.println("<button class='nav-btn' onclick='nav(2)'>🧩<br>AMX</button>");
    client.println("<button class='nav-btn' onclick='nav(3)'>🖥️<br>KIOS</button>");
    client.println("</div>");

    // ── PAGE 0: MEGA ──
    client.println("<div class='page' id='p0'>");
    client.println("<div class='top'><h2>🔌 MEGA CONTROL</h2><button class='info-btn' onclick='ti(0)'>ℹ️</button></div>");
    client.println("<div class='info-panel' id='i0'>");
    client.println("Browser → <code>ESP32 :80</code> → TCP :9000 → <code>MEGA 192.168.1.178</code> → RS485 → Board relay 16 kênh<br>Connect-per-transaction: mỗi lệnh mở TCP mới, gửi, đọc, đóng.");
    client.println("<div class='cfg'><span class='k'>MEGA IP</span><span class='v'>192.168.1.178:9000</span>");
    client.println("<span class='k'>Poll</span><span class='v'>2s</span><span class='k'>Timeout</span><span class='v'>300ms</span></div></div>");
    client.println("<div class='grid16'>");
    const char* names[] = {"Kho","H.Lang","P.Họp","Test Đèn","Lab","Còi","K.Doanh","N.Anh","P.Nguyên","P.Tuấn","Bàn Trà","Kế Toán","L13","L14","L15","L16"};
    for (int i = 1; i <= 16; i++) {
      client.print("<button id='L"); client.print(i); client.print("'>");
      client.print(i); client.print(". "); client.print(names[i-1]); client.println("</button>");
    }
    client.println("<button id='ALL' style='background:#6a1a1a;grid-column:1/-1'>🔴 TẮT TẤT CẢ</button></div>");
    client.println("<div class='sbar' id='sb0'>Đang tải...</div></div>");

    // ── PAGE 1: MODBUS / AC ──
    client.println("<div class='page' id='p1'>");
    client.println("<div class='top'><h2>❄️ ĐIỀU HÒA (LOGO! 8)</h2><button class='info-btn' onclick='ti(1)'>ℹ️</button></div>");
    client.println("<div class='info-panel' id='i1'>");
    client.println("Browser → <code>ESP32 :80</code> → Modbus TCP :504 → <code>LOGO! 8 192.168.1.6</code> → Relay Q1-Q4<br>");
    client.println("Pulse pattern: ghi Coil V0.4-V0.7 = ON → đợi 500ms → OFF. Feedback: đọc V0.0-V0.3 (FC1).");
    client.println("<div class='cfg'><span class='k'>LOGO! IP</span><span class='v'>192.168.1.6:504</span>");
    client.println("<span class='k'>Pulse</span><span class='v'>500ms</span><span class='k'>Cooldown</span><span class='v'>1500ms/kênh</span></div></div>");
    client.println("<div class='grid4'>");
    const char* acNames[] = {"ĐH1 Kế Toán","ĐH2 P.Nguyên","ĐH3 P.Họp","ĐH4 P.Tuấn"};
    for (int i = 0; i < 4; i++) {
      client.print("<button class='ac-btn' id='ac"); client.print(i); client.print("' onclick='acToggle("); client.print(i); client.println(")'>");
      client.print("<span class='icon'>❄️</span><span>"); client.print(acNames[i]); client.println("</span><div class='dot'></div></button>");
    }
    client.println("</div><div class='sbar' id='sb1'>Đang tải...</div></div>");

    // ── PAGE 2: AMX ──
    client.println("<div class='page' id='p2'>");
    client.println("<div class='top'><h2>🧩 AMX CONTROL</h2><button class='info-btn' onclick='ti(2)'>ℹ️</button></div>");
    client.println("<div class='info-panel' id='i2'>");
    client.println("Mỗi lần công tắc tường <b>thay đổi</b> → toggle relay. Chờ 300ms nhường Kramer → đọc relay thật → toggle nếu chưa đổi.");
    client.println("<div class='cfg'><span class='k'>CE-IO4</span><span class='v'>192.168.1.7:44197 (poll 600ms)</span>");
    client.println("<span class='k'>CE-REL8</span><span class='v'>192.168.1.204:44197</span></div></div>");
    client.println("<div class='section'><div class='section-title'>CE-REL8 — Relay đèn (1-4)</div>");
    const char* amxRelayNames[] = {"P.Họp","P.Tuấn","K.Doanh","H.Lang"};
    for (int i = 0; i < 4; i++) {
      client.print("<button class='relay-btn' id='R"); client.print(i+1); client.print("' onclick='amxToggle("); client.print(i+1); client.println(")'>");
      client.print("<span class='icon'>💡</span><span>Relay "); client.print(i+1); client.print(" — "); client.print(amxRelayNames[i]); client.println("</span></button>");
    }
    client.println("</div><div class='section'><div class='section-title'>CE-IO4 — Công tắc tường (1-4)</div><div class='io-grid'>");
    const char* amxIoNames[] = {"IO 1","IO 2","IO 3","IO 4"};
    for (int i = 0; i < 4; i++) {
      client.print("<div class='io-indicator' id='IO"); client.print(i+1); client.print("'>"); client.print(amxIoNames[i]); client.println("<div class='io-dot'></div></div>");
    }
    client.println("</div></div><div class='sbar' id='sb2'>Đang tải...</div></div>");

    // ── PAGE 3: KIOS ──
    client.println("<div class='page' id='p3' id='kios-page'>");
    client.println("<div class='kios-bar'>");
    client.println("<div class='site-btn' id='kb0' onclick='kLoad(0)'>KC-Brain</div>");
    client.println("<div class='site-btn' id='kb1' onclick='kLoad(1)'>SL-240C</div>");
    client.println("<div class='site-btn add' onclick='kToggleInput()'>＋ Nhập URL</div>");
    client.println("<div class='site-btn' style='margin-left:auto;background:#1a2a1a;border-color:#2a4a2a' onclick='kReload()'>🔄</div>");
    client.println("</div>");
    client.println("<div class='url-bar' id='kurl'>");
    client.println("<input id='url-input' type='text' placeholder='http://...'>");
    client.println("<button class='kbtn' style='background:#225588' onclick='kGo()'>▶ Mở</button>");
    client.println("<button class='kbtn' style='background:#333' onclick='kNewTab()'>↗ Tab</button></div>");
    client.println("<iframe id='kf' class='kios-frame' src=''></iframe></div>");

    // ── JAVASCRIPT ──
    client.println("<script>");
    // navigation
    client.println("var pages=4,cur=0;");
    client.println("function nav(i){");
    client.println("  document.querySelectorAll('.page').forEach(function(p,j){p.classList.toggle('active',j===i);});");
    client.println("  document.querySelectorAll('.nav-btn').forEach(function(b,j){b.classList.toggle('active',j===i);});");
    client.println("  cur=i;if(i===3&&!document.getElementById('kf').src)kLoad(0);");
    client.println("}");
    // info panel toggle
    client.println("function ti(n){var p=document.getElementById('i'+n);p.style.display=p.style.display==='block'?'none':'block';}");

    // ── MEGA logic ──
    client.println("var pendingM={},cmdFlight=false,lastCmd=0,qCmd=null;");
    client.println("function applyMega(t){t.split(',').forEach(function(p){var kv=p.trim().split('=');if(kv.length!=2)return;var k=kv[0].trim(),v=kv[1].trim();var b=document.getElementById(k);if(!b)return;var on=(v==='1');b.classList.toggle('on',on);pendingM[k]=on;});document.getElementById('sb0').innerText='Cập nhật: '+new Date().toLocaleTimeString();}");
    client.println("function sendM(c,b){var now=Date.now();if(now-lastCmd<150){qCmd={c,b};setTimeout(function(){if(!cmdFlight&&qCmd){var q=qCmd;qCmd=null;sendM(q.c,q.b);}},150);return;}lastCmd=now;cmdFlight=true;if(b){b.classList.add('pressing');setTimeout(function(){b.classList.remove('pressing');},90);}fetch('/cmd?c='+encodeURIComponent(c)).then(function(){setTimeout(pollMega,80);}).catch(function(){setTimeout(pollMega,150);}).finally(function(){cmdFlight=false;if(qCmd){var q=qCmd;qCmd=null;sendM(q.c,q.b);}});}");
    client.println("for(var _i=1;_i<=16;_i++){(function(i){var b=document.getElementById('L'+i);if(b)b.onclick=function(){var k='L'+i,cur=(k in pendingM)?pendingM[k]:b.classList.contains('on'),next=!cur;pendingM[k]=next;b.classList.toggle('on',next);sendM('set /relay/'+i+'/state '+(next?'true':'false'),b);};}(_i));}");
    client.println("document.getElementById('ALL').onclick=function(e){pendingM={};sendM('set /system/all off',e.target);};");
    client.println("function pollMega(){fetch('/status').then(function(r){return r.text();}).then(applyMega).catch(function(){document.getElementById('sb0').innerText='Lỗi kết nối MEGA';});}");

    // ── AC logic ──
    client.println("var COOL=1600,lastAC=[-1e9,-1e9,-1e9,-1e9];");
    client.println("function acToggle(ch){var now=Date.now();if(now-lastAC[ch]<COOL)return;lastAC[ch]=now;var b=document.getElementById('ac'+ch);b.classList.add('cooling');fetch('/ac/toggle?ch='+ch).then(function(){setTimeout(pollAC,700);}).catch(function(){setTimeout(pollAC,1000);});setTimeout(function(){b.classList.remove('cooling');},COOL);}");
    client.println("function pollAC(){fetch('/ac/status').then(function(r){return r.json();}).then(function(d){d.ac.forEach(function(v,i){document.getElementById('ac'+i).classList.toggle('on',!!v);});document.getElementById('sb1').innerText='Cập nhật: '+new Date().toLocaleTimeString();}).catch(function(){document.getElementById('sb1').innerText='Lỗi kết nối LOGO!';});}");

    // ── AMX logic ──
    client.println("var pendingR={};");
    client.println("function amxToggle(ch){var b=document.getElementById('R'+ch);var on=(ch in pendingR)?pendingR[ch]:b.classList.contains('on');var next=!on;pendingR[ch]=next;b.classList.toggle('on',next);fetch('/amx/relay?ch='+ch+'&value='+next).then(function(){setTimeout(pollAMX,300);}).catch(function(){setTimeout(pollAMX,500);});}");
    client.println("function pollAMX(){fetch('/amx/status').then(function(r){return r.json();}).then(function(d){d.relay.forEach(function(v,i){var b=document.getElementById('R'+(i+1));if(b){b.classList.toggle('on',!!v);pendingR[i+1]=!!v;}});d.io.forEach(function(v,i){var el=document.getElementById('IO'+(i+1));if(el)el.classList.toggle('active',!!v);});document.getElementById('sb2').innerText='Cập nhật: '+new Date().toLocaleTimeString();}).catch(function(){document.getElementById('sb2').innerText='Lỗi kết nối AMX';});}");

    // ── KIOS logic ──
    client.println("var kSites=['http://192.168.1.236:8000/','http://192.168.1.215:8000/'];");
    client.println("var kfr=document.getElementById('kf'),kinp=document.getElementById('url-input'),kub=document.getElementById('kurl');");
    client.println("function kLoad(i){kfr.src=kSites[i];kub.style.display='none';[0,1].forEach(function(j){document.getElementById('kb'+j).classList.toggle('active',j===i);});}");
    client.println("function kToggleInput(){kub.style.display=kub.style.display==='flex'?'none':'flex';if(kub.style.display==='flex')kinp.focus();}");
    client.println("function kGo(){var u=kinp.value.trim();if(!u)return;if(!u.startsWith('http'))u='http://'+u;kinp.value=u;kfr.src=u;}");
    client.println("function kNewTab(){var u=kfr.src||kinp.value.trim();if(u)window.open(u,'_blank');}");
    client.println("function kReload(){kfr.src=kfr.src;}");
    client.println("kinp.addEventListener('keydown',function(e){if(e.key==='Enter')kGo();});");

    // ── polling master ──
    // Chỉ poll trang đang active để tiết kiệm socket
    client.println("function masterPoll(){if(cur===0)pollMega();else if(cur===1)pollAC();else if(cur===2)pollAMX();}");
    client.println("setInterval(masterPoll,2000);");
    // Khởi động: load trang 0 và poll ngay
    client.println("nav(0);pollMega();");
    client.println("</script></body></html>");
    client.stop(); return;
  }

  /* ===== WEB UI MEGA ===== */
  if (request.startsWith("GET /mega ")) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\n");
    client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    client.println("<style>body{font-family:Arial;background:#111;color:#fff;text-align:center;}");
    client.println(".top{display:flex;gap:8px;align-items:center;padding:8px 12px;background:#1a1a1a;border-bottom:1px solid #333;flex-wrap:wrap;}");
    client.println(".back{padding:7px 12px;border-radius:8px;background:#333;color:#fff;text-decoration:none;font-size:13px;}");
    client.println(".back:hover{background:#444;} .top h2{margin:0;font-size:15px;flex:1;}");
    client.println(".info-btn{padding:6px 10px;border-radius:7px;background:#2a2a2a;color:#aaa;border:1px solid #444;cursor:pointer;font-size:13px;}");
    client.println(".info-btn:hover{background:#333;color:#fff;}");
    client.println(".info-panel{display:none;text-align:left;background:#181818;border-bottom:1px solid #333;padding:14px 16px;font-size:12px;line-height:1.7;color:#bbb;}");
    client.println(".info-panel h4{color:#fff;margin:0 0 6px;font-size:13px;} .info-panel code{color:#88ccff;background:#1a2a3a;padding:1px 5px;border-radius:4px;}");
    client.println(".info-panel .cfg{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin-top:8px;}");
    client.println(".cfg .k{color:#888;} .cfg .v{color:#7dd3fc;font-family:monospace;}");
    client.println(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(100px,1fr));gap:10px;padding:10px;}");
    client.println("button{height:60px;font-size:12px;border-radius:10px;border:none;background:#444;color:#fff;transition:0.1s;cursor:pointer;}");
    client.println("button:active{transform:scale(0.95);background:#666;}");
    client.println("button.pressing{transform:scale(0.95);background:#888;}");
    client.println(".on{background:green;color:#fff;} button.on{background:green;}");
    client.println("</style></head><body>");
    client.println("<div class='top'><a class='back' href='/'>⬅ Menu</a><h2>🔌 MEGA CONTROL</h2>");
    client.println("<button class='info-btn' onclick='toggleInfo()'>ℹ️ Thông tin</button></div>");
    client.println("<div class='info-panel' id='infop'>");
    client.println("<h4>Kiến trúc</h4>");
    client.println("Browser → HTTP :80 → <code>ESP32 192.168.1.180</code> → TCP :9000 → <code>MEGA 192.168.1.178</code> → RS485 9600 → <b>Board relay 16 kênh</b><br>");
    client.println("ESP32 dùng <b>connect-per-transaction</b>: mỗi lệnh mở TCP mới, gửi, đọc response, đóng. Không giữ persistent connection.<br>");
    client.println("Công tắc tường nối vào input vật lý của board relay, MEGA mirror thay đổi input → toggle relay tương ứng.");
    client.println("<h4 style='margin-top:10px'>Cấu hình hiện tại</h4><div class='cfg'>");
    client.println("<span class='k'>MEGA IP</span><span class='v'>192.168.1.178:9000</span>");
    client.println("<span class='k'>Poll status</span><span class='v'>mỗi 3s (ESP32 → MEGA get /relay/all)</span>");
    client.println("<span class='k'>Timeout</span><span class='v'>300ms</span>");
    client.println("</div></div>");
    client.println("<script>function toggleInfo(){var p=document.getElementById('infop');p.style.display=p.style.display==='block'?'none':'block';}</script>");
    client.println("<h4>RELAY CONTROL (1-16)</h4><div class='grid'>");
    const char* names[] = {"Kho","H.Lang","P.Họp","Test Đèn","Lab","Còi","K.Doanh","N.Anh","P.Nguyên","P.Tuấn","Bàn Trà","Kế Toán","L13","L14","L15","L16"};
    for (int i = 1; i <= 16; i++) {
      client.print("<button id='L"); client.print(i); client.print("'>");
      client.print(i); client.print(". "); client.print(names[i-1]); client.println("</button>");
    }
    client.println("<button id='ALL' style='background:#aa0000;'>🔴 TẮT TẤT CẢ</button></div>");
    client.println("<pre id='status' style='font-size:10px;margin-top:20px;opacity:0.5;'>Loading...</pre>");
    client.println("<script>");
    client.println("let lastCmdTime=0;let cmdInFlight=false;let queuedCmd=null;const CMD_COOLDOWN_MS=150;");
    client.println("let pendingTarget={};");
    client.println("function applyStatus(t){document.getElementById('status').innerText=t;t.split(',').forEach(p=>{let kv=p.split('=');if(kv.length!=2)return;let k=kv[0].trim();let v=kv[1].trim();let b=document.getElementById(k);if(!b)return;let isOn=(v=='1');if(isOn)b.classList.add('on');else b.classList.remove('on');pendingTarget[k]=isOn;});}");
    client.println("function sendCmd(c,btn){let now=Date.now();if(now-lastCmdTime<CMD_COOLDOWN_MS){queuedCmd={c,btn};setTimeout(()=>{if(!cmdInFlight&&queuedCmd){const q=queuedCmd;queuedCmd=null;sendCmd(q.c,q.btn);}},CMD_COOLDOWN_MS);return;}lastCmdTime=now;cmdInFlight=true;if(btn){btn.classList.add('pressing');setTimeout(()=>btn.classList.remove('pressing'),90);}fetch('/cmd?c='+encodeURIComponent(c)).then(()=>setTimeout(poll,80)).catch(()=>setTimeout(poll,150)).finally(()=>{cmdInFlight=false;if(queuedCmd){const q=queuedCmd;queuedCmd=null;sendCmd(q.c,q.btn);}});}");
    client.println("function cmd(c,btn){if(cmdInFlight){queuedCmd={c,btn};return;}sendCmd(c,btn);}");
    client.println("for(let i=1;i<=16;i++){let b=document.getElementById('L'+i);let key='L'+i;if(b)b.onclick=()=>{let current=(key in pendingTarget)?pendingTarget[key]:b.classList.contains('on');let next=!current;pendingTarget[key]=next;b.classList.toggle('on',next);cmd('set /relay/'+i+'/state '+(next?'true':'false'),b);};}");
    client.println("document.getElementById('ALL').onclick=(e)=>{pendingTarget={};cmd('set /system/all off',e.target);};");
    client.println("function poll(){fetch('/status').then(r=>r.text()).then(t=>applyStatus(t));}");
    client.println("setInterval(poll,2000);poll();");
    client.println("</script></body></html>");
    client.stop(); return;
  }

  // ===== AMX RELAY SET =====
  if (request.indexOf("GET /amx/relay?") >= 0) {
    int chPos = request.indexOf("ch=") + 3;
    int chEnd = request.indexOf("&", chPos); if (chEnd < 0) chEnd = request.indexOf(" ", chPos);
    int vPos  = request.indexOf("value=") + 6;
    int vEnd  = request.indexOf(" ", vPos);
    int ch = request.substring(chPos, chEnd).toInt();
    bool val = request.substring(vPos, vEnd) == "true";
    if (ch >= 1 && ch <= 4) {
      amxSetRelay(ch, val);
    }
    client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n");
    client.print("{\"ch\":"); client.print(ch);
    client.print(",\"value\":"); client.print(val?"true":"false"); client.println("}");
    client.stop(); return;
  }

  // ===== AMX STATUS =====
  if (request.indexOf("GET /amx/status") >= 0) {
    amxReadRelays();
    // amxInputSnapshot được cập nhật liên tục qua persistent connection + amxIoPoll()
    // không cần gọi amxReadInputs() TCP ở đây
    client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n");
    client.print("{\"relay\":[");
    for (int i = 0; i < 4; i++) { client.print((amxRelaySnapshot>>i)&1); if(i<3) client.print(","); }
    client.print("],\"io\":[");
    for (int i = 0; i < 4; i++) { client.print((amxInputSnapshot>>i)&1); if(i<3) client.print(","); }
    client.println("]}");
    client.stop(); return;
  }

  /* ===== WEB UI AMX ===== */
  if (request.startsWith("GET /amx ")) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\n");
    client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    client.println("<style>");
    client.println("body{font-family:Arial;background:#111;color:#fff;text-align:center;padding:0;}");
    client.println(".top{display:flex;align-items:center;gap:10px;padding:10px 14px;background:#1a1a1a;border-bottom:1px solid #333;flex-wrap:wrap;}");
    client.println(".back{padding:7px 12px;border-radius:8px;background:#333;color:#fff;text-decoration:none;font-size:14px;}");
    client.println(".back:hover{background:#444;} h2{margin:0;font-size:16px;flex:1;}");
    client.println(".section{max-width:600px;margin:20px auto;padding:0 14px;}");
    client.println(".section-title{text-align:left;font-size:12px;color:#888;text-transform:uppercase;margin-bottom:8px;letter-spacing:1px;}");
    client.println(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:12px;}");
    client.println(".relay-btn{height:80px;border-radius:12px;border:2px solid #444;background:#222;color:#fff;font-size:13px;font-weight:bold;cursor:pointer;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:5px;transition:0.15s;}");
    client.println(".relay-btn .icon{font-size:22px;} .relay-btn.on{background:#1a4a1a;border-color:#2d8a2d;color:#86efac;}");
    client.println(".relay-btn.on .icon{color:#86efac;} .relay-btn:active{transform:scale(0.96);}");
    client.println(".io-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-top:0;}");
    client.println(".io-indicator{padding:10px 6px;border-radius:10px;border:1px solid #333;background:#1a1a1a;font-size:11px;color:#888;}");
    client.println(".io-indicator.active{border-color:#55aaff;color:#55aaff;background:#0a1a2a;}");
    client.println(".io-dot{width:10px;height:10px;border-radius:50%;background:#333;margin:4px auto 0;}");
    client.println(".io-indicator.active .io-dot{background:#55aaff;}");
    client.println("#status-bar{font-size:11px;opacity:0.4;margin:12px 0;}");
    client.println(".info-btn{padding:6px 10px;border-radius:7px;background:#2a2a2a;color:#aaa;border:1px solid #444;cursor:pointer;font-size:13px;}");
    client.println(".info-btn:hover{background:#333;color:#fff;}");
    client.println(".info-panel{display:none;text-align:left;background:#181818;border-bottom:1px solid #333;padding:14px 16px;font-size:12px;line-height:1.7;color:#bbb;}");
    client.println(".info-panel h4{color:#fff;margin:0 0 6px;font-size:13px;} .info-panel code{color:#88ccff;background:#1a2a3a;padding:1px 5px;border-radius:4px;}");
    client.println(".info-panel .cfg{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin-top:8px;} .cfg .k{color:#888;} .cfg .v{color:#7dd3fc;font-family:monospace;}");
    client.println("</style></head><body>");
    client.println("<div class='top'><a class='back' href='/'>⬅ Menu</a><h2>🧩 AMX CONTROL</h2>");
    client.println("<button class='info-btn' onclick='toggleInfo()'>ℹ️ Thông tin</button></div>");
    client.println("<div class='info-panel' id='infop'>");
    client.println("<h4>Kiến trúc</h4>");
    client.println("Browser → HTTP :80 → <code>ESP32 192.168.1.180</code><br>");
    client.println("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;→ TCP :44197 → <code>CE-IO4 192.168.1.7</code> (4 công tắc tường, poll 800ms)<br>");
    client.println("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;→ TCP :44197 → <code>CE-REL8 192.168.1.204</code> (4 relay đèn)<br>");
    client.println("<h4 style='margin-top:8px'>Logic công tắc tường</h4>");
    client.println("Mỗi lần công tắc <b>thay đổi trạng thái</b> (bật→tắt hoặc tắt→bật) → toggle relay tương ứng.<br>");
    client.println("Chờ 500ms nhường Kramer phản hồi trước → đọc relay thật:<br>");
    client.println("&nbsp;• Relay chưa thay đổi → Kramer chưa xử lý → ESP32 toggle<br>");
    client.println("&nbsp;• Relay đã thay đổi &nbsp;→ Kramer đã xử lý &nbsp;→ ESP32 bỏ qua<br>");
    client.println("Nút web gọi <code>set /relay/N/state</code> trực tiếp, không liên quan đến logic toggle.");
    client.println("<h4 style='margin-top:8px'>Cấu hình</h4><div class='cfg'>");
    client.println("<span class='k'>CE-IO4 IP</span><span class='v'>192.168.1.7:44197</span>");
    client.println("<span class='k'>CE-REL8 IP</span><span class='v'>192.168.1.204:44197</span>");
    client.println("<span class='k'>IO poll</span><span class='v'>mỗi 800ms</span>");
    client.println("<span class='k'>Toggle delay</span><span class='v'>500ms (nhường Kramer)</span>");
    client.println("</div></div>");
    client.println("<script>function toggleInfo(){var p=document.getElementById('infop');p.style.display=p.style.display==='block'?'none':'block';}</script>");

    // Relay section
    client.println("<div class='section'><div class='section-title'>CE-REL8 — Điều khiển đèn (Relay 1-4)</div><div class='grid'>");
    const char* amxRelayNames[] = {"P.Họp","P.Tuấn","K.Doanh","H.Lang"};
    for (int i = 0; i < 4; i++) {
      client.print("<button class='relay-btn' id='R"); client.print(i+1); client.print("' onclick='toggleRelay("); client.print(i+1); client.println(")'>");
      client.println("<span class='icon'>💡</span>");
      client.print("<span>Relay "); client.print(i+1); client.print(" — "); client.print(amxRelayNames[i]); client.println("</span></button>");
    }
    client.println("</div></div>");

    // IO section
    client.println("<div class='section'><div class='section-title'>CE-IO4 — Công tắc tường (IO Input 1-4)</div><div class='io-grid'>");
    const char* amxIoNames[] = {"IO 1","IO 2","IO 3","IO 4"};
    for (int i = 0; i < 4; i++) {
      client.print("<div class='io-indicator' id='IO"); client.print(i+1); client.println("'>");
      client.print(amxIoNames[i]); client.println("<div class='io-dot'></div></div>");
    }
    client.println("</div></div>");
    client.println("<div id='status-bar'>Đang tải...</div>");

    client.println("<script>");
    client.println("let pendingRelay={};");
    client.println("function toggleRelay(ch){");
    client.println("  let btn=document.getElementById('R'+ch);");
    client.println("  let current=(ch in pendingRelay)?pendingRelay[ch]:btn.classList.contains('on');");
    client.println("  let next=!current; pendingRelay[ch]=next;");
    client.println("  btn.classList.toggle('on',next);");
    client.println("  fetch('/amx/relay?ch='+ch+'&value='+next).then(()=>setTimeout(poll,300)).catch(()=>setTimeout(poll,500));");
    client.println("}");
    client.println("function poll(){");
    client.println("  fetch('/amx/status').then(r=>r.json()).then(d=>{");
    client.println("    d.relay.forEach((v,i)=>{let b=document.getElementById('R'+(i+1));if(b){b.classList.toggle('on',!!v);pendingRelay[i+1]=!!v;}});");
    client.println("    d.io.forEach((v,i)=>{let el=document.getElementById('IO'+(i+1));if(el)el.classList.toggle('active',!!v);});");
    client.println("    document.getElementById('status-bar').innerText='Cập nhật: '+new Date().toLocaleTimeString();");
    client.println("  }).catch(()=>{document.getElementById('status-bar').innerText='Lỗi kết nối AMX';});");
    client.println("}");
    client.println("setInterval(poll,2000);poll();");
    client.println("</script></body></html>");
    client.stop(); return;
  }

  /* ===== WEB UI MODBUS — AC CONTROL ===== */
  if (request.startsWith("GET /modbus ")) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\n");
    client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    client.println("<style>");
    client.println("body{font-family:Arial;background:#111;color:#fff;text-align:center;padding:0;}");
    client.println(".top{display:flex;align-items:center;gap:10px;padding:10px 14px;background:#1a1a1a;border-bottom:1px solid #333;flex-wrap:wrap;}");
    client.println(".back{padding:7px 12px;border-radius:8px;background:#333;color:#fff;text-decoration:none;font-size:14px;}");
    client.println(".back:hover{background:#444;}");
    client.println("h2{margin:0;font-size:16px;flex:1;}");
    client.println(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:14px;max-width:600px;margin:30px auto;padding:0 16px;}");
    client.println(".ac-btn{position:relative;height:90px;border-radius:14px;border:2px solid #444;background:#222;color:#fff;font-size:14px;font-weight:bold;cursor:pointer;transition:0.15s;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;}");
    client.println(".ac-btn .icon{font-size:26px;}");
    client.println(".ac-btn.on{background:#0a4a7a;border-color:#1a8fd1;color:#7dd3fc;}");
    client.println(".ac-btn.on .icon{color:#7dd3fc;}");
    client.println(".ac-btn.cooling{opacity:0.5;pointer-events:none;}");
    client.println(".ac-btn:active{transform:scale(0.96);}");
    client.println(".dot{width:8px;height:8px;border-radius:50%;background:#555;margin-top:2px;}");
    client.println(".on .dot{background:#1a8fd1;}");
    client.println("#status-bar{font-size:11px;opacity:0.45;margin-top:10px;}");
    client.println("</style></head><body>");
    client.println("<style>.info-btn{padding:6px 10px;border-radius:7px;background:#2a2a2a;color:#aaa;border:1px solid #444;cursor:pointer;font-size:13px;}.info-btn:hover{background:#333;color:#fff;}");
    client.println(".info-panel{display:none;text-align:left;background:#181818;border-bottom:1px solid #333;padding:14px 16px;font-size:12px;line-height:1.7;color:#bbb;}");
    client.println(".info-panel h4{color:#fff;margin:0 0 6px;font-size:13px;} .info-panel code{color:#88ccff;background:#1a2a3a;padding:1px 5px;border-radius:4px;}");
    client.println(".info-panel .cfg{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin-top:8px;} .cfg .k{color:#888;} .cfg .v{color:#7dd3fc;font-family:monospace;}</style>");
    client.println("<div class='top'><a class='back' href='/'>⬅ Menu</a><h2>❄️ ĐIỀU HOÀ (LOGO! 8)</h2>");
    client.println("<button class='info-btn' onclick='toggleInfo()'>ℹ️ Thông tin</button></div>");
    client.println("<div class='info-panel' id='infop'>");
    client.println("<h4>Kiến trúc</h4>");
    client.println("Browser → HTTP :80 → <code>ESP32 192.168.1.180</code> → Modbus TCP :504 → <code>LOGO! 8 192.168.1.6</code> → Relay Q1-Q4 → Nút ĐH 1-4<br>");
    client.println("Điều khiển bằng <b>pulse pattern</b>: ghi Coil V0.4-V0.7 = ON → đợi 500ms → OFF. LOGO! phát hiện cạnh lên → B001-B004 tạo xung 1s → relay nhả → giả lập nhấn nút ĐH.<br>");
    client.println("Feedback thật: đọc Coil V0.0-V0.3 (FC1, addr 0-3) — LED bảng ĐH → I1-I4 → M21-M24 → NQ1-NQ4 → VM V0.0-V0.3.");
    client.println("<h4 style='margin-top:10px'>Cấu hình hiện tại</h4><div class='cfg'>");
    client.println("<span class='k'>LOGO! IP</span><span class='v'>192.168.1.6:504</span>");
    client.println("<span class='k'>Điều khiển coil</span><span class='v'>V0.4-V0.7 (PDU addr 4-7, FC5)</span>");
    client.println("<span class='k'>Feedback coil</span><span class='v'>V0.0-V0.3 (PDU addr 0-3, FC1)</span>");
    client.println("<span class='k'>Pulse width</span><span class='v'>500ms ON → OFF</span>");
    client.println("<span class='k'>Cooldown</span><span class='v'>1500ms / kênh</span>");
    client.println("<span class='k'>Lưu ý</span><span class='v'>Kramer đang dùng 1/8 slot Modbus TCP của LOGO!</span>");
    client.println("</div></div>");
    client.println("<script>function toggleInfo(){var p=document.getElementById('infop');p.style.display=p.style.display==='block'?'none':'block';}</script>");
    client.println("<div class='grid'>");
    const char* acNames[] = {"ĐH1 Kế Toán","ĐH2 P.Nguyên","ĐH3 P.Họp","ĐH4 P.Tuấn"};
    for (int i = 0; i < 4; i++) {
      client.print("<button class='ac-btn' id='ac"); client.print(i); client.print("' onclick='toggle("); client.print(i); client.println(")'>");
      client.println("<span class='icon'>❄️</span>");
      client.print("<span>"); client.print(acNames[i]); client.println("</span>");
      client.println("<div class='dot'></div></button>");
    }
    client.println("</div><div id='status-bar'>Đang tải...</div>");
    client.println("<script>");
    client.println("const COOLDOWN=1600;let lastToggle=[-Infinity,-Infinity,-Infinity,-Infinity];");
    client.println("function toggle(ch){");
    client.println("  let now=Date.now();");
    client.println("  if(now-lastToggle[ch]<COOLDOWN)return;");
    client.println("  lastToggle[ch]=now;");
    client.println("  let btn=document.getElementById('ac'+ch);");
    client.println("  btn.classList.add('cooling');");
    client.println("  fetch('/ac/toggle?ch='+ch).then(()=>setTimeout(poll,700)).catch(()=>setTimeout(poll,1000));");
    client.println("  setTimeout(()=>btn.classList.remove('cooling'),COOLDOWN);");
    client.println("}");
    client.println("function poll(){");
    client.println("  fetch('/ac/status').then(r=>r.json()).then(d=>{");
    client.println("    d.ac.forEach((v,i)=>{");
    client.println("      let b=document.getElementById('ac'+i);");
    client.println("      if(v)b.classList.add('on');else b.classList.remove('on');");
    client.println("    });");
    client.println("    document.getElementById('status-bar').innerText='Cập nhật: '+new Date().toLocaleTimeString();");
    client.println("  }).catch(()=>{document.getElementById('status-bar').innerText='Lỗi kết nối LOGO!';});");
    client.println("}");
    client.println("setInterval(poll,2000);poll();");
    client.println("</script></body></html>");
    client.stop(); return;
  }

  /* ===== WEB UI KIOS ===== */
  if (request.startsWith("GET /kios ")) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\n");
    client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    client.println("<style>*{margin:0;padding:0;box-sizing:border-box;}body{display:flex;flex-direction:column;height:100vh;background:#111;font-family:Arial;}");
    client.println(".top{display:flex;align-items:center;gap:8px;padding:7px 10px;background:#1a1a1a;border-bottom:1px solid #333;flex-shrink:0;flex-wrap:wrap;}");
    client.println(".back{padding:6px 10px;border-radius:7px;background:#333;color:#fff;text-decoration:none;font-size:13px;white-space:nowrap;}");
    client.println(".back:hover{background:#444;} .title{color:#fff;font-size:14px;font-weight:bold;flex:1;}");
    client.println(".site-bar{display:flex;gap:8px;padding:8px 10px;background:#111;border-bottom:1px solid #222;flex-shrink:0;flex-wrap:wrap;}");
    client.println(".site-btn{padding:7px 18px;border-radius:8px;border:1px solid #335;background:#1a1a2a;color:#aad4ff;font-size:13px;font-weight:bold;cursor:pointer;}");
    client.println(".site-btn:hover{background:#223;border-color:#558;color:#fff;}");
    client.println(".site-btn.active{background:#225588;border-color:#4488cc;color:#fff;}");
    client.println(".site-btn.add{background:#1a2a1a;border-color:#3a5a3a;color:#88cc88;}");
    client.println(".site-btn.add:hover{background:#223322;color:#aaffaa;}");
    client.println(".url-bar{display:none;align-items:center;gap:6px;padding:7px 10px;background:#141414;border-bottom:1px solid #2a2a2a;flex-shrink:0;flex-wrap:wrap;}");
    client.println("#url-input{flex:1;min-width:200px;padding:6px 10px;border-radius:7px;border:1px solid #444;background:#222;color:#fff;font-size:13px;}");
    client.println(".btn{padding:6px 12px;border-radius:7px;color:#fff;border:none;cursor:pointer;font-size:13px;white-space:nowrap;}");
    client.println(".btn-go{background:#225588;} .btn-go:hover{background:#2a66aa;}");
    client.println(".hint{background:#1a1a2a;border-bottom:1px solid #2a2a44;padding:6px 12px;font-size:11px;color:#7799cc;flex-shrink:0;display:none;}");
    client.println("iframe{flex:1;width:100%;border:none;background:#fff;}</style></head><body>");
    client.println("<div class='top'><a class='back' href='/'>⬅ Menu</a><span class='title'>🖥️ KIOS</span>");
    client.println("<button class='btn' style='background:#1a3a1a;border:1px solid #2a5a2a;margin-left:auto;' onclick='reloadFrame()'>🔄 Tải lại</button>");
    client.println("<button class='btn' style='background:#3a1a1a;border:1px solid #5a2a2a;' onclick='window.location.reload()'>↺ Khởi động lại</button></div>");
    client.println("<div class='site-bar'>");
    client.println("<div class='site-btn' id='btn0' onclick='loadSite(0)'>KC-Brain</div>");
    client.println("<div class='site-btn' id='btn1' onclick='loadSite(1)'>SL-240C</div>");
    client.println("<div class='site-btn add' id='btn2' onclick='toggleInput()'>＋ Nhập trang mới</div>");
    client.println("</div>");
    client.println("<div class='url-bar' id='url-bar'>");
    client.println("<input id='url-input' type='text' placeholder='http://...' />");
    client.println("<button class='btn btn-go' onclick='goCustom()'>▶ Mở</button>");
    client.println("<button class='btn' style='background:#333;' onclick='openNew()'>↗ Tab mới</button></div>");
    client.println("<div class='hint' id='hint'></div>");
    client.println("<iframe id='kf' src=''></iframe>");
    client.println("<script>");
    client.println("var sites=['http://192.168.1.236:8000/','http://192.168.1.215:8000/'];");
    client.println("var fr=document.getElementById('kf'),ub=document.getElementById('url-bar'),hint=document.getElementById('hint');");
    client.println("var inp=document.getElementById('url-input');");
    client.println("var cur=-1;");
    client.println("function loadSite(i){cur=i;fr.src=sites[i];ub.style.display='none';hint.style.display='block';");
    client.println("  [0,1,2].forEach(function(j){document.getElementById('btn'+j).classList.toggle('active',j===i);});");
    client.println("}");
    client.println("function toggleInput(){ub.style.display=ub.style.display==='flex'?'none':'flex';");
    client.println("  if(ub.style.display==='flex'){inp.focus();hint.style.display='block';}");
    client.println("  [0,1].forEach(function(j){document.getElementById('btn'+j).classList.remove('active');});");
    client.println("}");
    client.println("function goCustom(){var u=inp.value.trim();if(!u)return;if(!u.startsWith('http'))u='http://'+u;");
    client.println("  inp.value=u;fr.src=u;localStorage.setItem('kios_custom',u);}");
    client.println("function openNew(){var u=fr.src||inp.value.trim();if(u)window.open(u,'_blank');}");
    client.println("function reloadFrame(){if(fr.src)fr.src=fr.src;}");
    client.println("inp.addEventListener('keydown',function(e){if(e.key==='Enter')goCustom();});");
    // Auto-reconnect: ping ESP32 mỗi 10 giây, nếu mất rồi có lại thì tự reload trang
    client.println("var _lost=false;");
    client.println("setInterval(function(){");
    client.println("  fetch('/').then(function(){if(_lost){_lost=false;window.location.reload();}})");
    client.println("  .catch(function(){_lost=true;});");
    client.println("},10000);");
    client.println("loadSite(0);");
    client.println("</script></body></html>");
    client.stop(); return;
  }

  /* ===== CMD ===== */
  if (request.indexOf("GET /cmd?") >= 0) {
    int s = request.indexOf("c=") + 2;
    int e = request.indexOf(" ", s);
    String cmd = request.substring(s, e);
    cmd.replace("%20", " "); cmd.replace("%2F", "/");
    megaTransact(cmd);  // connect → send → read status → disconnect
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n");
    client.println("OK");
    client.stop(); return;
  }

  if (request.indexOf("GET /status") >= 0) {
    // Trả cached globalStatus — được cập nhật bởi megaTransact() sau mỗi lệnh
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n" + globalStatus);
    client.stop(); return;
  }

  client.stop();
}

/* ===== SETUP ===== */
void setup() {
  Serial.begin(115200);
  Serial.print("ESP32 Firmware v"); Serial.println(FW_VERSION);

  resetW5500();
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, W5500_CS);
  SPI.setFrequency(8000000);
  Ethernet.init(W5500_CS);
  Ethernet.begin(mac, ip, dns, gateway, subnet);
  delay(1000);

  Serial.print("IP: "); Serial.println(Ethernet.localIP());
  server.begin();

  // Đọc trạng thái ban đầu từ MEGA
  megaTransact("get /relay/all");

  // Khởi tạo CE-IO4 inputMode
  amxIoConnect();

  // NTP sync lần đầu (retry sẽ chạy trong loop() nếu thất bại)
  if (ntpSync()) {
    Serial.println("NTP OK");
  } else {
    Serial.println("NTP failed — will retry in loop()");
  }
}

/* ===== LOOP ===== */
void loop() {

  EthernetClient client = server.available();
  if (client) handleWebRequest(client);

  // LOGO! pulse completion — non-blocking
  if (logoPulse.active && millis() - logoPulse.startMs >= LOGO_PULSE_MS) {
    logoFC5(LOGO_M_ADDR + logoPulse.channel, false);
    logoPulse.active = false;
  }

  // Sync trạng thái relay định kỳ (phòng trường hợp thay đổi từ công tắc vật lý / scene)
  static unsigned long lastSync = 0;
  if (millis() - lastSync >= 3000) {
    lastSync = millis();
    megaTransact("get /relay/all");
  }

  // AMX CE-IO4 — persistent connection: đọc response non-blocking mỗi vòng loop,
  // gửi get commands mỗi AMX_IO_POLL_INTERVAL_MS. Reconnect nếu mất kết nối.
  if (amxIoClient.connected()) {
    amxIoPoll();  // non-blocking read
    static unsigned long lastIoPoll = 0;
    if (millis() - lastIoPoll >= AMX_IO_POLL_INTERVAL_MS) {
      lastIoPoll = millis();
      amxIoSendGet();
    }
  } else if (millis() - amxIoLastOk >= AMX_IO_RECONNECT_MS) {
    amxIoLastOk = millis();
    amxIoConnect();
  }

  // Reconcile IO→relay sau delay (nhường Kramer phản hồi trước)
  if (amxNeedsReconcile && millis() >= amxReconcileAfter) {
    amxNeedsReconcile = false;
    amxReconcile();
  }

  // NTP re-sync và scheduler
  static unsigned long lastSchedCheck = 0;
  if (millis() - lastSchedCheck >= 1000) {
    lastSchedCheck = millis();
    // Re-sync NTP định kỳ hoặc retry nếu chưa sync
    unsigned long resyncInterval = ntpSynced ? NTP_RESYNC_MS : NTP_RETRY_MS;
    if (millis() - lastNtpAttempt >= resyncInterval) {
      lastNtpAttempt = millis();
      ntpSync();
    }
    runScheduler();
  }
}
