#include <SPI.h>
#include <Ethernet.h>

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

/* ===== SERVER ===== */
EthernetServer server(80);
EthernetClient megaClient;
String globalStatus = "";


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

/* ===== HTTP HANDLER ===== */
void handleWebRequest(EthernetClient client) {
  String request = "";
  while (client.available()) {
    char c = client.read();
    request += c;
    if (request.indexOf("\r\n\r\n") >= 0) break;
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

    client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n");
    client.print("{\"status\":\"success\",\"relay\":"); client.print(relayNum);
    client.print(",\"value\":"); client.print(isOn ? "true" : "false");
    client.println("}");
    client.stop();
    return;
  }

  // ===== API STATUS JSON =====
  if (request.indexOf("GET /api/status") >= 0) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n");
    client.print("{\"raw\":\""); client.print(globalStatus); client.println("\"}");
    client.stop();
    return;
  }

  /* ===== WEB UI MENU ===== */
  if (request.startsWith("GET / ")) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\n");
    client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    client.println("<style>body{font-family:Arial;background:#111;color:#fff;text-align:center;padding:20px;}h2{margin-top:10px}.menu{max-width:500px;margin:30px auto;display:grid;gap:14px;}a{display:block;padding:18px;border-radius:12px;background:#333;color:#fff;text-decoration:none;font-size:18px;border:1px solid #555;}a:hover{background:#444;}</style></head><body>");
    client.println("<h2>TNN SI - SMART CONTROL</h2>");
    client.println("<p>Chọn nhóm thiết bị cần điều khiển:</p>");
    client.println("<div class='menu'>");
    client.println("<a href='/mega'>🔌 Điều khiển thiết bị Mega</a>");
    client.println("<a href='/amx'>🧩 Điều khiển thiết bị AMX</a>");
    client.println("</div></body></html>");
    client.stop();
    return;
  }

  /* ===== WEB UI MEGA (OLD PAGE) ===== */
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
    client.println("</style></head><body>");

    client.println("<div class='top'><a class='back' href='/'>⬅ Menu</a><h2>TNN SI - MEGA CONTROL</h2></div>");

    client.println("<h4>RELAY CONTROL (1-16)</h4><div class='grid'>");
    const char* names[] = {"Kho","H.Lang","P.Họp","Test Đèn","Lab","Còi","K.Doanh","N.Anh","P.Nguyên","P.Tuấn","Bàn Trà","Kế Toán","L13","L14","L15","L16"};
    for(int i=1; i<=16; i++) {
      client.print("<button id='L"); client.print(i); client.print("'>");
      client.print(i); client.print(". "); client.print(names[i-1]); client.println("</button>");
    }
    client.println("<button id='ALL' style='background:#aa0000;'>🔴 TẮT TẤT CẢ</button></div>");

    client.println("<h4>EXPANSION / AC CONTROL</h4><div class='grid'>");
    client.println("<button id='AC1' class='ac'>13. Kế toán (P)</button>");
    client.println("<button id='AC2' class='ac'>14. P.Nguyên (P)</button>");
    client.println("<button id='AC3' class='ac'>15. P.Họp (P)</button>");
    client.println("<button id='AC4' class='ac'>16. P.Tuấn (P)</button></div>");

    client.println("<pre id='status' style='font-size:10px;margin-top:20px;opacity:0.5;'>Loading...</pre>");

    client.println("<script>");
    client.println("let lastCmdTime=0;let cmdInFlight=false;const CMD_COOLDOWN_MS=120;");
    client.println("function cmd(c,btn){let now=Date.now();if(cmdInFlight)return;if(now-lastCmdTime<CMD_COOLDOWN_MS)return;lastCmdTime=now;cmdInFlight=true;if(btn){btn.classList.add('pressing');setTimeout(()=>btn.classList.remove('pressing'),120);}fetch('/cmd?c='+encodeURIComponent(c)).then(()=>setTimeout(poll,120)).catch(()=>{}).finally(()=>{cmdInFlight=false;});}");
    client.println("for(let i=1;i<=16;i++){let b=document.getElementById('L'+i);if(b)b.onclick=()=>cmd('set /relay/'+i+'/toggle',b);}");
    client.println("for(let i=1;i<=4;i++){let b=document.getElementById('AC'+i);if(b)b.onclick=()=>cmd('set /ac/'+i+'/pulse',b);}");
    client.println("document.getElementById('ALL').onclick=(e)=>cmd('set /system/all off',e.target);");
    client.println("function poll(){fetch('/status').then(r=>r.text()).then(t=>{document.getElementById('status').innerText=t;t.split(',').forEach(p=>{let kv=p.split('=');if(kv.length!=2)return;let k=kv[0].trim();let v=kv[1].trim();let b=document.getElementById(k);if(!b)return;if(v=='1')b.classList.add('on');else b.classList.remove('on');});});}");
    client.println("setInterval(poll,2000);poll();");
    client.println("</script></body></html>");

    client.stop();
    return;
  }

  /* ===== WEB UI AMX (NEW PAGE PLACEHOLDER) ===== */
  if (request.startsWith("GET /amx ")) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\n");
    client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    client.println("<style>body{font-family:Arial;background:#111;color:#fff;text-align:center;padding:16px;}.top{display:flex;justify-content:center;gap:10px;align-items:center;flex-wrap:wrap;}.back{display:inline-block;padding:8px 12px;border-radius:8px;background:#333;color:#fff;text-decoration:none;}.grid{display:grid;grid-template-columns:repeat(2,minmax(120px,1fr));gap:10px;max-width:500px;margin:20px auto;}button{height:70px;font-size:14px;border-radius:10px;border:none;background:#225588;color:#fff;}small{opacity:.75;display:block;margin-top:8px;}</style></head><body>");
    client.println("<div class='top'><a class='back' href='/'>⬅ Menu</a><h2>AMX CONTROL</h2></div>");
    client.println("<p>Trang giao diện AMX (khung cơ bản 4 nút, sẽ gắn lệnh sau).</p>");
    client.println("<div class='grid'>");
    client.println("<button id='AMX1'>AMX 1</button>");
    client.println("<button id='AMX2'>AMX 2</button>");
    client.println("<button id='AMX3'>AMX 3</button>");
    client.println("<button id='AMX4'>AMX 4</button>");
    client.println("</div><small>Hiện tại các nút đang ở chế độ placeholder.</small>");
    client.println("</body></html>");
    client.stop();
    return;
  }

  /* ===== CMD ===== */
  if (request.indexOf("GET /cmd?") >= 0) {
    int s = request.indexOf("c=") + 2;
    int e = request.indexOf(" ", s);
    String cmd = request.substring(s, e);
    cmd.replace("%20", " "); cmd.replace("%2F", "/");
    sendToMega(cmd);
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nOK");
    client.stop();
    return;
  }

  if (request.indexOf("GET /status") >= 0) {
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n" + globalStatus);
    client.stop();
    return;
  }

  client.stop();
}

/* ===== SETUP ===== */
void setup() {
  Serial.begin(115200);

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

  EthernetClient client = server.available();
  if (client) {
    handleWebRequest(client);
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

  if (millis() - t > 3000) {   // 🔥 giảm tần suất reconnect
    t = millis();

    Serial.println("Attempting to reconnect to Mega...");

    if (megaClient.connect(megaIP, megaPort)) {

      Serial.println("RECONNECTED OK");

      delay(200); // 🔥 tăng delay cho chắc

      megaClient.print("HELLO ESP\r\n");

      delay(200);

      sendToMega("get /relay/all");
    }
  }
}
}
