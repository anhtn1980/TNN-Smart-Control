#include <SPI.h>
#include <Ethernet.h>

/* ===== FIRMWARE VERSION ===== */
#define FW_VERSION "1.5.0"

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
// v1.1.0: thời gian tối đa chờ đủ header HTTP trước khi bỏ cuộc
#define REQUEST_TIMEOUT_MS 500

/* ===== HTTP KEEP-ALIVE CONFIG (v1.5.0) ===== */
// Số kết nối HTTP đồng thời tối đa được giữ mở (keep-alive). W5500 (cấu
// hình mặc định thư viện Ethernet) chỉ có 4 socket phần cứng; 1 dành cho
// server lắng nghe, 1 dành cho megaClient (kết nối thường trực tới MEGA),
// nên chỉ còn tối đa 2 socket cho client web — đặt đúng bằng giới hạn đó
// để không vượt quá khả năng phần cứng.
#define MAX_WEB_CLIENTS 2
// Đóng kết nối nếu không có hoạt động gì trong khoảng thời gian này (dọn
// dẹp các tab trình duyệt bị bỏ quên/đã đóng mà không báo hiệu rõ ràng).
// Phải lớn hơn chu kỳ poll() định kỳ của JS (2000ms) để không tự đóng kết
// nối đang dùng bình thường.
#define WEB_IDLE_TIMEOUT_MS 8000

/* ===== SERVER ===== */
EthernetServer server(80);
EthernetClient megaClient;
String globalStatus = "";

// v1.5.0: giữ một vài kết nối HTTP mở lâu dài (keep-alive) thay vì mở mới
// + đóng ngay sau mỗi request — giảm số lần mở/đóng socket trên W5500 khi
// bấm nút dồn dập, tránh tình trạng socket bị quá tải khiến kết nối
// megaClient (TCP tới MEGA) bị rớt liên tục.
EthernetClient webClients[MAX_WEB_CLIENTS];
unsigned long webLastActivity[MAX_WEB_CLIENTS] = {0};


String normalizeStatusLine(const String &raw) {
  int start = raw.lastIndexOf("L1=");
  if (start < 0) return "";

  String part = raw.substring(start);
  part.replace("\r", "");
  part.replace("\n", "");

  int end = part.lastIndexOf("L16=");
  if (end < 0) return part;

  int commaAfter = part.indexOf(',', end);
  if (commaAfter > 0) {
    return part.substring(0, commaAfter);
  }
  return part;
}

/* ===== RESET W5500 ===== */
void resetW5500() {
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, HIGH);
  delay(100);
  digitalWrite(W5500_RST, LOW);
  delay(200);
  digitalWrite(W5500_RST, HIGH);
  delay(200);
}

/* ===== TCP SEND ===== */
void sendToMega(String cmd) {
  if (!megaClient.connected()) return;
  Serial.println("TX: " + cmd);
  megaClient.print(cmd + "\r\n");
}

/* ===== HTTP RESPONSE HELPER (v1.5.0) ===== */
// Gửi response kèm Content-Length chính xác + Connection: keep-alive,
// KHÔNG đóng kết nối — bắt buộc phải có Content-Length đúng để trình
// duyệt biết ranh giới response trên 1 kết nối được tái sử dụng nhiều lần,
// nếu không trình duyệt sẽ treo chờ thêm dữ liệu hoặc đọc lẫn sang response
// kế tiếp.
void sendResponse(EthernetClient &client, const char* status, const char* contentType, const String &body) {
  client.print("HTTP/1.1 ");
  client.print(status);
  client.print("\r\nContent-Type: ");
  client.print(contentType);
  client.print("\r\nContent-Length: ");
  client.print(body.length());
  client.print("\r\nConnection: keep-alive\r\n\r\n");
  client.print(body);
}

/* ===== ACCEPT WEB CLIENTS (v1.5.0) ===== */
void acceptWebClients() {
  EthernetClient nc = server.available();
  if (!nc) return;

  // server.available() có thể trả về MỘT KẾT NỐI ĐÃ ĐƯỢC THEO DÕI SẴN
  // (không chỉ kết nối mới) nếu nó đang có dữ liệu chờ đọc — kiểm tra
  // trùng lặp trước, tránh gán cùng 1 socket vào 2 slot khác nhau (sẽ gây
  // xung đột khi đọc dữ liệu hoặc đóng nhầm kết nối đang dùng).
  for (int i = 0; i < MAX_WEB_CLIENTS; i++) {
    if (webClients[i] && webClients[i] == nc) return;
  }

  for (int i = 0; i < MAX_WEB_CLIENTS; i++) {
    if (!webClients[i] || !webClients[i].connected()) {
      webClients[i] = nc;
      webLastActivity[i] = millis();
      return;
    }
  }
  // Hết slot — không thể giữ thêm, đóng kết nối mới này luôn.
  nc.stop();
}

/* ===== HTTP HANDLER ===== */
// v1.5.0: nhận EthernetClient theo tham chiếu, không còn gọi client.stop()
// ở các nhánh xử lý thành công — kết nối được giữ mở (keep-alive) để dùng
// lại cho các request tiếp theo trên cùng 1 socket.
void handleWebRequest(EthernetClient &client) {
  String request = "";
  bool headerComplete = false;
  unsigned long startWait = millis();

  while (!headerComplete && (millis() - startWait < REQUEST_TIMEOUT_MS)) {
    while (client.available()) {
      char c = client.read();
      request += c;
      if (request.indexOf("\r\n\r\n") >= 0) {
        headerComplete = true;
        break;
      }
    }
    if (headerComplete) break;
    if (!client.connected()) break;
  }

  if (!headerComplete) {
    client.stop();
    return;
  }

  // ===== API SET =====
  if (request.indexOf("GET /set?") >= 0) {
    int rPos = request.indexOf("relay=") + 6;
    int vPos = request.indexOf("value=") + 6;
    int rEnd = request.indexOf("&", rPos);
    int vEnd = request.indexOf(" ", vPos);
    if (rEnd == -1) rEnd = request.indexOf(" ", rPos);

    String relayNum = request.substring(rPos, rEnd);
    String valStr = request.substring(vPos, vEnd);
    bool isOn = (valStr == "true" || valStr == "1");

    sendToMega("set /relay/" + relayNum + "/state " + (isOn ? "true" : "false"));

    String body = "{\"status\":\"success\",\"relay\":" + relayNum + ",\"value\":" + (isOn ? "true" : "false") + "}";
    sendResponse(client, "200 OK", "application/json", body);
    return;
  }

  // ===== API STATUS JSON =====
  if (request.indexOf("GET /api/status") >= 0) {
    String body = "{\"raw\":\"" + globalStatus + "\"}";
    sendResponse(client, "200 OK", "application/json", body);
    return;
  }

  /* ===== WEB UI MENU ===== */
  if (request.startsWith("GET / ")) {
    String body = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<style>body{font-family:Arial;background:#111;color:#fff;text-align:center;padding:20px;}h2{margin-top:10px}.menu{max-width:500px;margin:30px auto;display:grid;gap:14px;}a{display:block;padding:18px;border-radius:12px;background:#333;color:#fff;text-decoration:none;font-size:18px;border:1px solid #555;}a:hover{background:#444;}</style></head><body>"
      "<h2>TNN SI - SMART CONTROL</h2>"
      "<p>Chọn nhóm thiết bị cần điều khiển:</p>"
      "<div class='menu'>"
      "<a href='/mega'>🔌 Điều khiển thiết bị Mega</a>"
      "<a href='/amx'>🧩 Điều khiển thiết bị AMX</a>"
      "</div></body></html>";
    sendResponse(client, "200 OK", "text/html; charset=UTF-8", body);
    return;
  }

  /* ===== WEB UI MEGA (OLD PAGE) ===== */
  if (request.startsWith("GET /mega ")) {
    String body = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
    body += "<style>";
    body += "body{font-family:Arial;background:#111;color:#fff;text-align:center;}";
    body += ".top{display:flex;justify-content:center;gap:10px;align-items:center;flex-wrap:wrap;}";
    body += ".back{display:inline-block;padding:8px 12px;border-radius:8px;background:#333;color:#fff;text-decoration:none;}";
    body += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(100px,1fr));gap:10px;}";
    body += "button{height:60px;font-size:12px;border-radius:10px;border:none;background:#444;color:#fff;transition:0.1s;cursor:pointer;}";
    body += "button:active{transform:scale(0.95);background:#666;}";
    body += "button.pressing{transform:scale(0.95);background:#888;}";
    body += ".on{background:green;color:#fff;}";
    body += ".ac{background:#225588;}";
    body += "</style></head><body>";

    body += "<div class='top'><a class='back' href='/'>⬅ Menu</a><h2>TNN SI - MEGA CONTROL</h2></div>";

    body += "<h4>RELAY CONTROL (1-16)</h4><div class='grid'>";
    const char* names[] = {"Kho","H.Lang","P.Họp","Test Đèn","Lab","Còi","K.Doanh","N.Anh","P.Nguyên","P.Tuấn","Bàn Trà","Kế Toán","L13","L14","L15","L16"};
    for (int i = 1; i <= 16; i++) {
      body += "<button id='L"; body += i; body += "'>";
      body += i; body += ". "; body += names[i-1]; body += "</button>";
    }
    body += "<button id='ALL' style='background:#aa0000;'>🔴 TẮT TẤT CẢ</button></div>";

    body += "<h4>EXPANSION / AC CONTROL</h4><div class='grid'>";
    body += "<button id='AC1' class='ac'>13. Kế toán (P)</button>";
    body += "<button id='AC2' class='ac'>14. P.Nguyên (P)</button>";
    body += "<button id='AC3' class='ac'>15. P.Họp (P)</button>";
    body += "<button id='AC4' class='ac'>16. P.Tuấn (P)</button></div>";

    body += "<pre id='status' style='font-size:10px;margin-top:20px;opacity:0.5;'>Loading...</pre>";

    body += "<script>";
    body += "let lastCmdTime=0;let cmdInFlight=false;let queuedCmd=null;const CMD_COOLDOWN_MS=90;";
    body += "let pendingTarget={};";
    body += "function applyStatus(t){document.getElementById('status').innerText=t;t.split(',').forEach(p=>{let kv=p.split('=');if(kv.length!=2)return;let k=kv[0].trim();let v=kv[1].trim();let b=document.getElementById(k);if(!b)return;let isOn=(v=='1');if(isOn)b.classList.add('on');else b.classList.remove('on');pendingTarget[k]=isOn;});}";
    body += "function sendCmd(c,btn){let now=Date.now();if(now-lastCmdTime<CMD_COOLDOWN_MS){queuedCmd={c,btn};setTimeout(()=>{if(!cmdInFlight&&queuedCmd){const q=queuedCmd;queuedCmd=null;sendCmd(q.c,q.btn);}},CMD_COOLDOWN_MS);return;}lastCmdTime=now;cmdInFlight=true;if(btn){btn.classList.add('pressing');setTimeout(()=>btn.classList.remove('pressing'),90);}fetch('/cmd?c='+encodeURIComponent(c)).then(()=>setTimeout(poll,80)).catch(()=>setTimeout(poll,150)).finally(()=>{cmdInFlight=false;if(queuedCmd){const q=queuedCmd;queuedCmd=null;sendCmd(q.c,q.btn);}});}";
    body += "function cmd(c,btn){if(cmdInFlight){queuedCmd={c,btn};return;}sendCmd(c,btn);}";
    body += "for(let i=1;i<=16;i++){let b=document.getElementById('L'+i);let key='L'+i;if(b)b.onclick=()=>{let current=(key in pendingTarget)?pendingTarget[key]:b.classList.contains('on');let next=!current;pendingTarget[key]=next;b.classList.toggle('on',next);cmd('set /relay/'+i+'/state '+(next?'true':'false'),b);};}";
    body += "for(let i=1;i<=4;i++){let b=document.getElementById('AC'+i);if(b)b.onclick=()=>cmd('set /ac/'+i+'/pulse',b);}";
    body += "document.getElementById('ALL').onclick=(e)=>{pendingTarget={};cmd('set /system/all off',e.target);};";
    body += "function poll(){fetch('/status').then(r=>r.text()).then(t=>applyStatus(t));}";
    body += "setInterval(poll,2000);poll();";
    body += "</script></body></html>";

    sendResponse(client, "200 OK", "text/html; charset=UTF-8", body);
    return;
  }

  /* ===== WEB UI AMX (NEW PAGE PLACEHOLDER) ===== */
  if (request.startsWith("GET /amx ")) {
    String body = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<style>body{font-family:Arial;background:#111;color:#fff;text-align:center;padding:16px;}.top{display:flex;justify-content:center;gap:10px;align-items:center;flex-wrap:wrap;}.back{display:inline-block;padding:8px 12px;border-radius:8px;background:#333;color:#fff;text-decoration:none;}.grid{display:grid;grid-template-columns:repeat(2,minmax(120px,1fr));gap:10px;max-width:500px;margin:20px auto;}button{height:70px;font-size:14px;border-radius:10px;border:none;background:#225588;color:#fff;}small{opacity:.75;display:block;margin-top:8px;}</style></head><body>"
      "<div class='top'><a class='back' href='/'>⬅ Menu</a><h2>AMX CONTROL</h2></div>"
      "<p>Trang giao diện AMX (khung cơ bản 4 nút, sẽ gắn lệnh sau).</p>"
      "<div class='grid'>"
      "<button id='AMX1'>AMX 1</button>"
      "<button id='AMX2'>AMX 2</button>"
      "<button id='AMX3'>AMX 3</button>"
      "<button id='AMX4'>AMX 4</button>"
      "</div><small>Hiện tại các nút đang ở chế độ placeholder.</small>"
      "</body></html>";
    sendResponse(client, "200 OK", "text/html; charset=UTF-8", body);
    return;
  }

  /* ===== CMD ===== */
  if (request.indexOf("GET /cmd?") >= 0) {
    int s = request.indexOf("c=") + 2;
    int e = request.indexOf(" ", s);
    String cmd = request.substring(s, e);
    cmd.replace("%20", " "); cmd.replace("%2F", "/");
    sendToMega(cmd);

    sendResponse(client, "200 OK", "text/plain", "OK");
    return;
  }

  if (request.indexOf("GET /status") >= 0) {
    sendResponse(client, "200 OK", "text/plain", globalStatus);
    return;
  }

  // v1.5.0: route không khớp (vd. /favicon.ico) — PHẢI trả lời (kể cả rỗng)
  // thay vì im lặng như trước, vì giờ kết nối được giữ mở (keep-alive); nếu
  // không phản hồi, trình duyệt sẽ treo chờ mãi trên cùng kết nối, làm kẹt
  // luôn các request tiếp theo xếp hàng sau nó.
  sendResponse(client, "404 Not Found", "text/plain", "");
}

/* ===== SETUP ===== */
void setup() {
  Serial.begin(115200);
  Serial.print("ESP32 Firmware v");
  Serial.println(FW_VERSION);

  resetW5500();

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, W5500_CS);
  SPI.setFrequency(8000000);

  Ethernet.init(W5500_CS);
  Ethernet.begin(mac, ip, dns, gateway, subnet);

  delay(1000);

  Serial.print("IP: ");
  Serial.println(Ethernet.localIP());

  server.begin();

  megaClient.connect(megaIP, megaPort);
  delay(100);
  megaClient.print("HELLO ESP\r\n");
  delay(100);
  sendToMega("get /relay/all");
}

/* ===== LOOP ===== */
void loop() {

  acceptWebClients();

  for (int i = 0; i < MAX_WEB_CLIENTS; i++) {
    if (webClients[i] && webClients[i].connected()) {
      if (webClients[i].available()) {
        webLastActivity[i] = millis();
        handleWebRequest(webClients[i]);
      } else if (millis() - webLastActivity[i] > WEB_IDLE_TIMEOUT_MS) {
        webClients[i].stop();
      }
    } else if (webClients[i]) {
      webClients[i].stop();
    }
  }

  if (megaClient.connected() && megaClient.available()) {
    String line = megaClient.readStringUntil('\n');
    line.trim();
    String normalized = normalizeStatusLine(line);
    if (normalized.length() > 0) {
      globalStatus = normalized;
    }
  }

static unsigned long t = 0;

if (!megaClient.connected()) {

  if (millis() - t > 3000) {
    t = millis();

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
}
