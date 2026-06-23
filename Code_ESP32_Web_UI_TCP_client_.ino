#include <SPI.h>
#include <Ethernet.h>

/* ===== FIRMWARE VERSION ===== */
#define FW_VERSION "1.8.1"

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

/* ===== HTTP REQUEST READ CONFIG ===== */
#define REQUEST_TIMEOUT_MS 500

/* ===== LOGO! MODBUS TCP CONFIG (v1.8.0) ===== */
// Model: 6ED1052-1FB08-0BA1 (LOGO! 8 0BA8) tại 192.168.1.6:502
//
// Kiến trúc điều khiển điều hòa trong LOGO!:
//   Xung vào M1-M4 → B001-B004 (edge wiping relay 1s) → Q1-Q4 → relay nhấn nút ĐH
//   Feedback: I1-I4 → M21-M24 → NQ1-NQ4 → VM V0.0-V0.3
//
// Modbus address (theo tài liệu LOGO! 8):
//   ĐIỀU KHIỂN : M1-M4 = Coil 8257-8260 (1-based) → PDU address 8256-8259 (0-based)
//   FEEDBACK   : V0.0-V0.3 = Coil 1-4 (1-based) → PDU address 0-3 (0-based)
//
// Cách điều khiển: Write M bit ON → chờ 500ms (non-blocking) → Write M bit OFF
//   LOGO! tự phát hiện cạnh → tạo xung chuẩn 1s → nhả relay → giả lập nhấn nút ĐH
//
// Nếu không hoạt động: thử đổi LOGO_M_ADDR sang 8257 (zero-based vs one-based ambiguity)
const char* logoIP = "192.168.1.6";
const int   logoPort = 502;
#define LOGO_UNIT_ID        1
#define LOGO_M_ADDR         8256  // PDU 0-based address cho M1 (Coil 8257 theo tài liệu)
#define LOGO_FB_ADDR        0     // PDU 0-based address cho V0.0 (Coil 1 theo tài liệu)
#define LOGO_PULSE_MS       500   // thời gian giữ M bit = ON (đủ để LOGO phát hiện cạnh)
#define LOGO_COOLDOWN_MS    1500  // tối thiểu giữa 2 lần bấm cùng kênh
#define LOGO_MODBUS_TIMEOUT_MS 200
#define LOGO_POLL_MS        2500  // chu kỳ đọc feedback từ LOGO!

/* ===== SERVER ===== */
EthernetServer server(80);
EthernetClient megaClient;
EthernetClient logoClient;
String globalStatus = "";
uint8_t acSnapshot = 0;        // bits 0-3 = trạng thái thật AC1-AC4 từ V0.0-V0.3

/* ===== LOGO! PULSE STATE MACHINE (v1.8.0) ===== */
// Non-blocking: bật M bit ON ngay, state machine tắt OFF sau LOGO_PULSE_MS.
// Không cần delay() block loop() → ESP32 vẫn xử lý request khác trong khi đợi.
struct LogoPulse {
  bool active;
  int channel;            // 0-3
  unsigned long startMs;
};
LogoPulse logoPulse = {false, -1, 0};
unsigned long acCooldownMs[4] = {0, 0, 0, 0};


String normalizeStatusLine(const String &raw) {
  int start = raw.lastIndexOf("L1=");
  if (start < 0) return "";
  String part = raw.substring(start);
  part.replace("\r", "");
  part.replace("\n", "");
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

/* ===== TCP SEND TO MEGA ===== */
void sendToMega(String cmd) {
  if (!megaClient.connected()) return;
  Serial.println("TX: " + cmd);
  megaClient.print(cmd + "\r\n");
}

/* ===== MODBUS TCP LOW LEVEL (v1.8.0) ===== */
// Gửi FC5 Write Single Coil và drain response (không quan tâm nội dung response).
void logoFC5(uint16_t addr, bool val) {
  if (!logoClient.connected()) return;
  byte frame[12] = {
    0x00, 0x01, 0x00, 0x00, 0x00, 0x06,
    LOGO_UNIT_ID, 0x05,
    (byte)(addr >> 8), (byte)(addr & 0xFF),
    (byte)(val ? 0xFF : 0x00), 0x00
  };
  logoClient.write(frame, 12);
  // FC5 echo response = 12 bytes, drain để không làm nhiễu lần đọc kế tiếp
  // yield() bên trong để nhường CPU cho watchdog và task hệ thống ESP32
  unsigned long t0 = millis();
  while (logoClient.available() < 12 && millis() - t0 < LOGO_MODBUS_TIMEOUT_MS) { yield(); }
  while (logoClient.available()) logoClient.read();
}

// Gửi lệnh toggle cho kênh điều hòa ch (0-3):
//   - Ghi M bit ON ngay lập tức (non-blocking)
//   - Ghi OFF sau LOGO_PULSE_MS ms (do state machine trong loop() xử lý)
//   - Có per-channel cooldown và chống pulse chồng nhau
void logoSendPulse(int ch) {
  if (!logoClient.connected()) {
    Serial.print("LOGO! not connected, skip pulse AC"); Serial.println(ch + 1);
    return;
  }
  if (logoPulse.active) {
    Serial.println("LOGO! pulse in progress, skip");
    return;
  }
  if (millis() - acCooldownMs[ch] < LOGO_COOLDOWN_MS) {
    Serial.print("AC"); Serial.print(ch + 1); Serial.println(" cooldown, skip");
    return;
  }
  uint16_t addr = LOGO_M_ADDR + ch;  // M1=8256, M2=8257, M3=8258, M4=8259
  logoFC5(addr, true);   // bật M bit ON để LOGO phát hiện cạnh
  logoPulse.active  = true;
  logoPulse.channel = ch;
  logoPulse.startMs = millis();
  acCooldownMs[ch]  = millis();
  Serial.print("LOGO! pulse start M"); Serial.print(ch + 1);
  Serial.print(" addr=0x"); Serial.println(addr, HEX);
}

// FC1 Read Coils đọc V0.0-V0.3 (feedback trạng thái thật của 4 điều hòa)
void logoReadFeedback() {
  if (!logoClient.connected()) return;
  byte frame[12] = {
    0x00, 0x02, 0x00, 0x00, 0x00, 0x06,
    LOGO_UNIT_ID, 0x01,
    (byte)(LOGO_FB_ADDR >> 8), (byte)(LOGO_FB_ADDR & 0xFF),
    0x00, 0x04   // đọc 4 coils: V0.0-V0.3
  };
  logoClient.write(frame, 12);
  // Response: MBAP(6) + UnitID(1) + FC(1) + ByteCount(1) + CoilByte(1) = 10 bytes
  // yield() bên trong để nhường CPU cho watchdog và task hệ thống ESP32
  unsigned long t0 = millis();
  while (logoClient.available() < 10 && millis() - t0 < LOGO_MODBUS_TIMEOUT_MS) { yield(); }
  if (logoClient.available() >= 10) {
    byte resp[10];
    logoClient.readBytes(resp, 10);
    // resp[7]=FC=0x01, resp[8]=byte count=1, resp[9]=coil data (bit0=V0.0, bit1=V0.1,...)
    if (resp[7] == 0x01 && resp[8] == 0x01) {
      acSnapshot = resp[9] & 0x0F;
    }
  }
  while (logoClient.available()) logoClient.read();
}

/* ===== HTTP HANDLER ===== */
void handleWebRequest(EthernetClient client) {
  String request = "";
  bool headerComplete = false;
  unsigned long startWait = millis();

  while (!headerComplete && (millis() - startWait < REQUEST_TIMEOUT_MS)) {
    while (client.available()) {
      char c = client.read();
      request += c;
      if (request.indexOf("\r\n\r\n") >= 0) { headerComplete = true; break; }
    }
    if (headerComplete) break;
    if (!client.connected()) break;
  }
  if (!headerComplete) { client.stop(); return; }

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
    sendToMega("set /relay/" + relayNum + "/state " + (isOn ? "true" : "false"));
    client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n");
    client.print("{\"status\":\"success\",\"relay\":"); client.print(relayNum);
    client.print(",\"value\":"); client.print(isOn ? "true" : "false");
    client.println("}");
    client.stop(); return;
  }

  // ===== API STATUS JSON =====
  if (request.indexOf("GET /api/status") >= 0) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n");
    client.print("{\"raw\":\""); client.print(globalStatus); client.println("\"}");
    client.stop(); return;
  }

  /* ===== WEB UI MENU ===== */
  if (request.startsWith("GET / ")) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\n");
    client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    client.println("<style>body{font-family:Arial;background:#111;color:#fff;text-align:center;padding:20px;}h2{margin-top:10px}.menu{max-width:500px;margin:30px auto;display:grid;gap:14px;}a{display:block;padding:18px;border-radius:12px;background:#333;color:#fff;text-decoration:none;font-size:18px;border:1px solid #555;}a:hover{background:#444;}</style></head><body>");
    client.println("<h2>TNN SI - SMART CONTROL</h2><p>Chọn nhóm thiết bị cần điều khiển:</p>");
    client.println("<div class='menu'>");
    client.println("<a href='/mega'>🔌 Điều khiển thiết bị Mega</a>");
    client.println("<a href='/amx'>🧩 Điều khiển thiết bị AMX</a>");
    client.println("</div></body></html>");
    client.stop(); return;
  }

  /* ===== WEB UI MEGA ===== */
  if (request.startsWith("GET /mega ")) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\n");
    client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    client.println("<style>");
    client.println("body{font-family:Arial;background:#111;color:#fff;text-align:center;}");
    client.println(".top{display:flex;justify-content:center;gap:10px;align-items:center;flex-wrap:wrap;}");
    client.println(".back{display:inline-block;padding:8px 12px;border-radius:8px;background:#333;color:#fff;text-decoration:none;}");
    client.println(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(100px,1fr));gap:10px;}");
    client.println("button{height:60px;font-size:12px;border-radius:10px;border:none;background:#444;color:#fff;transition:0.1s;cursor:pointer;}");
    client.println("button:active{transform:scale(0.95);background:#666;}");
    client.println("button.pressing{transform:scale(0.95);background:#888;}");
    client.println(".on{background:green;color:#fff;}");
    client.println(".ac{background:#225588;}");
    // button.on phải đặt sau .ac để override (specificity 11 > 11, order wins)
    client.println("button.on{background:green;}");
    client.println("</style></head><body>");

    client.println("<div class='top'><a class='back' href='/'>⬅ Menu</a><h2>TNN SI - MEGA CONTROL</h2></div>");

    client.println("<h4>RELAY CONTROL (1-16)</h4><div class='grid'>");
    const char* names[] = {"Kho","H.Lang","P.Họp","Test Đèn","Lab","Còi","K.Doanh","N.Anh","P.Nguyên","P.Tuấn","Bàn Trà","Kế Toán","L13","L14","L15","L16"};
    for (int i = 1; i <= 16; i++) {
      client.print("<button id='L"); client.print(i); client.print("'>");
      client.print(i); client.print(". "); client.print(names[i-1]); client.println("</button>");
    }
    client.println("<button id='ALL' style='background:#aa0000;'>🔴 TẮT TẤT CẢ</button></div>");

    client.println("<h4>EXPANSION / AC CONTROL</h4><div class='grid'>");
    client.println("<button id='AC1' class='ac'>ĐH1. Kế toán</button>");
    client.println("<button id='AC2' class='ac'>ĐH2. P.Nguyên</button>");
    client.println("<button id='AC3' class='ac'>ĐH3. P.Họp</button>");
    client.println("<button id='AC4' class='ac'>ĐH4. P.Tuấn</button></div>");

    client.println("<pre id='status' style='font-size:10px;margin-top:20px;opacity:0.5;'>Loading...</pre>");

    client.println("<script>");
    client.println("let lastCmdTime=0;let cmdInFlight=false;let queuedCmd=null;const CMD_COOLDOWN_MS=150;");
    client.println("let pendingTarget={};");
    client.println("function applyStatus(t){document.getElementById('status').innerText=t;t.split(',').forEach(p=>{let kv=p.split('=');if(kv.length!=2)return;let k=kv[0].trim();let v=kv[1].trim();let b=document.getElementById(k);if(!b)return;let isOn=(v=='1');if(isOn)b.classList.add('on');else b.classList.remove('on');pendingTarget[k]=isOn;});}");
    client.println("function sendCmd(c,btn){let now=Date.now();if(now-lastCmdTime<CMD_COOLDOWN_MS){queuedCmd={c,btn};setTimeout(()=>{if(!cmdInFlight&&queuedCmd){const q=queuedCmd;queuedCmd=null;sendCmd(q.c,q.btn);}},CMD_COOLDOWN_MS);return;}lastCmdTime=now;cmdInFlight=true;if(btn){btn.classList.add('pressing');setTimeout(()=>btn.classList.remove('pressing'),90);}fetch('/cmd?c='+encodeURIComponent(c)).then(()=>setTimeout(poll,80)).catch(()=>setTimeout(poll,150)).finally(()=>{cmdInFlight=false;if(queuedCmd){const q=queuedCmd;queuedCmd=null;sendCmd(q.c,q.btn);}});}");
    client.println("function cmd(c,btn){if(cmdInFlight){queuedCmd={c,btn};return;}sendCmd(c,btn);}");
    // Relay buttons: toggle ON/OFF theo state thật
    client.println("for(let i=1;i<=16;i++){let b=document.getElementById('L'+i);let key='L'+i;if(b)b.onclick=()=>{let current=(key in pendingTarget)?pendingTarget[key]:b.classList.contains('on');let next=!current;pendingTarget[key]=next;b.classList.toggle('on',next);cmd('set /relay/'+i+'/state '+(next?'true':'false'),b);};}");
    // AC buttons: gửi xung toggle (không quan tâm ON/OFF vì LOGO! tự toggle)
    // Optimistic: đảo màu ngay để phản hồi người dùng, applyStatus() sẽ sửa lại đúng sau 2.5s
    client.println("for(let i=1;i<=4;i++){let b=document.getElementById('AC'+i);let key='AC'+i;if(b)b.onclick=()=>{let current=(key in pendingTarget)?pendingTarget[key]:b.classList.contains('on');let next=!current;pendingTarget[key]=next;b.classList.toggle('on',next);cmd('set /ac/'+i+'/pulse',b);};}");
    client.println("document.getElementById('ALL').onclick=(e)=>{pendingTarget={};cmd('set /system/all off',e.target);};");
    client.println("function poll(){fetch('/status').then(r=>r.text()).then(t=>applyStatus(t));}");
    client.println("setInterval(poll,2000);poll();");
    client.println("</script></body></html>");
    client.stop(); return;
  }

  /* ===== WEB UI AMX ===== */
  if (request.startsWith("GET /amx ")) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\n");
    client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    client.println("<style>body{font-family:Arial;background:#111;color:#fff;text-align:center;padding:16px;}.top{display:flex;justify-content:center;gap:10px;align-items:center;flex-wrap:wrap;}.back{display:inline-block;padding:8px 12px;border-radius:8px;background:#333;color:#fff;text-decoration:none;}.grid{display:grid;grid-template-columns:repeat(2,minmax(120px,1fr));gap:10px;max-width:500px;margin:20px auto;}button{height:70px;font-size:14px;border-radius:10px;border:none;background:#225588;color:#fff;}small{opacity:.75;display:block;margin-top:8px;}</style></head><body>");
    client.println("<div class='top'><a class='back' href='/'>⬅ Menu</a><h2>AMX CONTROL</h2></div>");
    client.println("<p>Trang giao diện AMX (khung cơ bản 4 nút, sẽ gắn lệnh sau).</p>");
    client.println("<div class='grid'><button id='AMX1'>AMX 1</button><button id='AMX2'>AMX 2</button><button id='AMX3'>AMX 3</button><button id='AMX4'>AMX 4</button></div>");
    client.println("<small>Hiện tại các nút đang ở chế độ placeholder.</small></body></html>");
    client.stop(); return;
  }

  /* ===== CMD ===== */
  if (request.indexOf("GET /cmd?") >= 0) {
    int s = request.indexOf("c=") + 2;
    int e = request.indexOf(" ", s);
    String cmd = request.substring(s, e);
    cmd.replace("%20", " "); cmd.replace("%2F", "/");

    if (cmd.startsWith("set /ac/")) {
      // v1.8.0: lệnh AC → gửi xung Modbus tới M bit của LOGO!
      // Format: "set /ac/1/pulse" (state ON/OFF không dùng, LOGO! tự toggle)
      int slashPos = cmd.indexOf('/', 8);
      if (slashPos > 0) {
        int ch = atoi(cmd.substring(8, slashPos).c_str()) - 1; // 0-based
        if (ch >= 0 && ch < 4) logoSendPulse(ch);
      }
    } else {
      sendToMega(cmd);
    }

    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n");
    client.println("OK");
    client.stop(); return;
  }

  if (request.indexOf("GET /status") >= 0) {
    // Nối AC1-AC4 (feedback từ LOGO! V0.0-V0.3) vào sau L1-L16 (từ MEGA)
    String fullStatus = globalStatus;
    for (int i = 0; i < 4; i++) {
      if (fullStatus.length() > 0) fullStatus += ",";
      fullStatus += "AC"; fullStatus += (i + 1);
      fullStatus += "="; fullStatus += bitRead(acSnapshot, i) ? "1" : "0";
    }
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n" + fullStatus);
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

  megaClient.connect(megaIP, megaPort);
  delay(100);
  megaClient.print("HELLO ESP\r\n");
  delay(100);
  sendToMega("get /relay/all");

  // v1.8.1: KHÔNG connect tới LOGO! trong setup() — logoClient.connect() có thể
  // block lâu nếu LOGO! chưa bật Modbus/không phản hồi, gây Watchdog Timer reset.
  // Kết nối sẽ được thử trong loop() mỗi 5s, an toàn hơn nhiều.
  Serial.println("LOGO! will connect in loop()");
}

/* ===== LOOP ===== */
void loop() {

  EthernetClient client = server.available();
  if (client) handleWebRequest(client);

  // Đọc trạng thái relay từ MEGA
  if (megaClient.connected() && megaClient.available()) {
    String line = megaClient.readStringUntil('\n');
    line.trim();
    String normalized = normalizeStatusLine(line);
    if (normalized.length() > 0) globalStatus = normalized;
  }

  // Reconnect MEGA
  static unsigned long tMega = 0;
  if (!megaClient.connected()) {
    if (millis() - tMega > 3000) {
      tMega = millis();
      Serial.println("Attempting to reconnect to Mega...");
      if (megaClient.connect(megaIP, megaPort)) {
        Serial.println("RECONNECTED OK");
        delay(200);
        megaClient.print("HELLO ESP\r\n");
        delay(200);
        sendToMega("get /relay/all");
      }
    }
  }

  // v1.8.0: hoàn tất xung M bit (tắt OFF sau LOGO_PULSE_MS)
  // Non-blocking: không dùng delay(), kiểm tra millis() mỗi vòng loop()
  if (logoPulse.active && millis() - logoPulse.startMs >= LOGO_PULSE_MS) {
    uint16_t addr = LOGO_M_ADDR + logoPulse.channel;
    logoFC5(addr, false);   // tắt M bit OFF → LOGO đã phát hiện cạnh, xung 1s đang chạy
    Serial.print("LOGO! pulse end M"); Serial.println(logoPulse.channel + 1);
    logoPulse.active = false;
  }

  // Reconnect LOGO!
  static unsigned long tLogo = 0;
  if (!logoClient.connected()) {
    if (millis() - tLogo > 5000) {
      tLogo = millis();
      Serial.println("Attempting to reconnect to LOGO!...");
      if (logoClient.connect(logoIP, logoPort)) {
        Serial.println("LOGO! RECONNECTED OK");
        delay(100);
        logoReadFeedback();
      }
    }
  }

  // Poll trạng thái AC từ LOGO! định kỳ
  static unsigned long lastLogoPoll = 0;
  if (logoClient.connected() && millis() - lastLogoPoll >= LOGO_POLL_MS) {
    lastLogoPoll = millis();
    logoReadFeedback();
  }
}
