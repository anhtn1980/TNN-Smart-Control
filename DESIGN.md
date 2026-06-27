# TNN Smart Control — Tài liệu thiết kế tổng thể

> **Mục đích**: Tài liệu "sống" phục vụ đồng thời cho tác giả và Claude
> trong các phiên làm việc tiếp theo. Cập nhật mỗi khi có thay đổi kiến trúc,
> quyết định thiết kế quan trọng, hoặc lỗi đã sửa cần ghi nhớ lý do.
>
> Lần cập nhật gần nhất: v3.2.0 — 2026-06-27

---

## 1. Bối cảnh & bài toán gốc

Văn phòng TNN SI cần điều khiển hệ thống đèn và điều hòa không khí từ xa qua
mạng LAN nội bộ, không phụ thuộc WiFi, hỗ trợ cả PC và điện thoại.

**Ràng buộc phần cứng**:
- Board relay RS485 16 kênh (giao thức Modbus-like, FC06 toggle/all-off, FC03 đọc trạng thái)
- Siemens LOGO! 8 0BA8 (`6ED1052-1FB08-0BA1`) điều khiển 4 điều hòa qua relay output Q1-Q4
- Công tắc gắn tường nối vào input vật lý của board relay

---

## 2. Kiến trúc tổng thể

```
[PC / Điện thoại]
       │
    HTTP :80
       │
[ESP32 + W5500]  192.168.1.180
       │              │                │               │
    TCP :9000    Modbus TCP :504    TCP :44197      TCP :44197
       │              │                │               │
[MEGA2560+W5500] [LOGO! 8]        [CE-IO4]        [CE-REL8]
 192.168.1.178  192.168.1.6      192.168.1.7    192.168.1.204
       │
    RS485 9600
       │
[Board relay 16 kênh]
```

### Vai trò từng thiết bị

| Thiết bị | Vai trò | Firmware/IP |
|---|---|---|
| ESP32 + W5500 | Web server port 80, HTTP API, giao diện web | v3.2.0 / 192.168.1.180 |
| MEGA2560 + W5500 | TCP server port 9000, điều khiển relay qua RS485 | v1.5.0 / 192.168.1.178 |
| LOGO! 8 | Modbus TCP server port 504, điều khiển 4 điều hòa | — / 192.168.1.6 |
| AMX CE-IO4 | 4 công tắc tường (digital input), TCP port 44197 | — / 192.168.1.7 |
| AMX CE-REL8 | 4 relay đèn, TCP port 44197 | — / 192.168.1.204 |
| AMX CE-IRS4 | Cảm biến hồng ngoại (dự phòng) | — / 192.168.1.203 |

---

## 3. Kiến trúc ESP32 (Web Server + Gateway)

### 3.1 Quyết định thiết kế quan trọng nhất: connect-per-transaction (v2.0.0)

**Vấn đề**: Từ v1.0 đến v1.9.x, ESP32 giữ một persistent TCP connection tới
MEGA (`megaClient`). Kết nối này liên tục bị drop khi người dùng bấm nút dồn
dập, gây ra vòng lặp `RECONNECTED OK` và nút bấm không nhạy.

**Nguyên nhân gốc**: W5500 chỉ có 8 socket phần cứng, chia sẻ cho: server
lắng nghe, megaClient persistent, và nhiều HTTP request socket từ browser.
Khi browser gửi nhiều request nhanh (poll /status + bấm nút), các socket
mở/đóng dồn dập gây xung đột socket management, drop megaClient theo.

**Giải pháp**: Bỏ persistent megaClient. Mỗi lệnh dùng `megaTransact()`:
mở TCP mới → gửi lệnh → đọc response → đóng. Giống pattern đã dùng cho LOGO!.

```
Browser bấm nút
  → fetch('/cmd?c=set /relay/4/state true')
  → ESP32: megaTransact("set /relay/4/state true")
      → TCP connect tới 192.168.1.178:9000
      → gửi lệnh
      → đọc response (L1=0,...,L16=0)  ← cập nhật globalStatus ngay
      → TCP disconnect
  → trả "OK" về browser
  → browser gọi setTimeout(poll, 80ms)
  → fetch('/status') → trả globalStatus (cached, không connect MEGA)
```

**Kết quả**: Bấm dồn dập bao nhiêu cũng không drop kết nối vì không có
kết nối nào để drop.

### 3.2 HTTP Routes

#### HTML (SPA — v2.6.0)

| Route | Mô tả |
|---|---|
| `GET /` | **SPA duy nhất** — trả toàn bộ HTML gồm 4 trang con (Đèn / Điều hòa / AMX / KIOS). Chuyển tab bằng JS, không gửi HTTP request mới. |

Trước v2.6.0 có 5 route riêng: `GET /`, `/mega`, `/modbus`, `/amx`, `/kios` — đã gộp vào SPA để tránh socket W5500 cạn khi chuyển trang liên tục (đặc biệt trên kiosk).

#### API

| Route | Mô tả |
|---|---|
| `GET /cmd?c=<lệnh>` | Gửi lệnh tới MEGA qua `megaTransact()` |
| `GET /status` | Trả cached `globalStatus` (L1=0,...,L16=0) |
| `GET /ac/toggle?ch=N` | Toggle điều hòa N (1-4) qua LOGO! pulse pattern |
| `GET /ac/status` | JSON trạng thái 4 điều hòa từ LOGO! feedback V0.0-V0.3 |
| `GET /amx/relay?ch=N&value=true\|false` | Set relay CE-REL8 kênh N |
| `GET /amx/status` | JSON: `{"relay":[...],"io":[...]}` trạng thái CE-REL8 + CE-IO4 |
| `GET /info` | JSON: `{fw, uptime, time, ntp, sched}` — debug từ xa |
| `GET /settings` | JSON cấu hình scheduler hiện tại |
| `POST /settings` | Lưu `enabled`, `hour`, `min` — ghi vào NVS flash |
| `POST /login` | Kiểm tra password, set cookie `sid`, redirect `/` |
| `GET /logout` | Xóa cookie, redirect `/login` |

### 3.3 SPA Architecture (v2.6.0)

```
GET / → ESP32 trả một HTML duy nhất (~900 dòng)
         ┌──────────────────────────────────────────────┐
         │  Nav bar: [Đèn] [Điều hòa] [AMX] [KIOS]     │
         │  <div id="p0" class="page active"> … </div>  │  ← Đèn (MEGA 16 relay)
         │  <div id="p1" class="page">        … </div>  │  ← Điều hòa (LOGO! AC)
         │  <div id="p2" class="page">        … </div>  │  ← AMX (CE-REL8 + CE-IO4)
         │  <div id="p3" class="page">        … </div>  │  ← KIOS (iframe browser)
         └──────────────────────────────────────────────┘

showPage(n) → ẩn trang cũ, hiện trang n (chỉ thay CSS, không fetch HTTP)

masterPoll() mỗi 2s:
  cur===0 → pollMega()     → GET /status
  cur===1 → pollAC()       → GET /ac/status
  cur===2 → pollAMX()      → GET /amx/status
  cur===3 → (không poll)   ← KIOS dùng iframe tự quản lý
```

**Lợi ích**: Không mở socket mới khi chuyển tab → giải quyết triệt để ERR_CONNECTION_REFUSED trên kiosk fullscreen.

### 3.4 JavaScript UI (trang Đèn)

**Vấn đề**: Bấm 2 lần nhanh liên tiếp → 2 lần tự triệt tiêu (v1.3.0).

**Nguyên nhân**: `onclick` đọc trực tiếp `classList.contains('on')` mỗi lần
bấm. Lần 2 đọc màu vừa bị lần 1 đổi, đảo ngược lại, net effect = không làm gì.

**Giải pháp**: `pendingTarget{}` — object lưu trạng thái đích dự kiến riêng
cho từng nút. Mỗi lần bấm: đọc `pendingTarget[key]` (không đọc DOM), đảo,
ghi lại, gửi lệnh. `applyStatus()` đồng bộ `pendingTarget` theo trạng thái
thật khi nhận data từ server.

### 3.5 Cấu hình mạng ESP32

```
IP tĩnh : 192.168.1.180
MAC     : DE:AD:BE:EF:FE:ED
W5500 CS: GPIO5, RST: GPIO4
SPI     : SCK=18, MISO=19, MOSI=23
```

---

## 4. Kiến trúc MEGA2560 (RS485 Controller)

### 4.1 Luồng xử lý

```
loop() {
  handleSceneSwitch()   ← nút cứng pin 30, debounce 50ms
  acceptClients()       ← TCP server port 9000, tối đa 4 client
  handleTCP()           ← đọc lệnh text, execCommand(), trả status
  [IO Poll mỗi 120ms]   ← readIO() đọc input+output qua RS485 (2x delay 60ms)
  [Sync nếu cần]        ← 150ms sau lệnh relay, đọc lại trạng thái thật
}
```

### 4.2 Giao thức lệnh TCP (text)

| Lệnh | Hành động |
|---|---|
| `set /relay/<1-16>/state true\|false` | Bật/tắt relay theo trạng thái đích |
| `set /relay/<1-16>/toggle` | Đảo trạng thái relay |
| `set /system/all off` | Tắt tất cả relay (frame `relayAllOff`) |
| `get /relay/all` | Đọc lại trạng thái, trả response |

MEGA luôn trả về 1 dòng status sau mỗi lệnh: `L1=0,L2=1,...,L16=0\n`

### 4.3 RS485 — Frame điều khiển relay

Board relay dùng giao thức Modbus-like (FC06 = write single register):
- `relayToggle[i]`: frame 8 byte toggle kênh i (không phải ON/OFF chuẩn Modbus,
  dùng giá trị `0x0300` — đặc thù của board relay giá rẻ này)
- `relayAllOff`: frame tắt tất cả (`0x0800`)

**Lưu ý**: Không dùng `waitForBytes()` để chờ phản hồi RS485 — RS485 là
bus bán song công, cần `delay(60)` cố định sau khi gửi để bus ổn định trước
khi đọc. Chờ chủ động bắt trúng nhiễu trên bus khi chuyển driver enable.

### 4.4 Mirror công tắc vật lý

`mirrorInputs()`: so sánh `inBits` mới với `lastInputs`. Mỗi bit thay đổi
→ gửi RS485 toggle kênh tương ứng. `mirrorBlockUntil` ngăn mirror đảo ngược
lại ngay sau khi relay vừa được điều khiển qua mạng.

### 4.5 Scene switch (pin 30)

Bấm 1 lần: nếu có đèn nào đang bật → tắt tất cả. Nếu tất cả đang tắt →
khôi phục `DEFAULT_LIGHT_MASK` (hiện tại: kênh 12 = "Kế Toán").

### 4.6 Cấu hình mạng MEGA

```
IP tĩnh : 192.168.1.178
MAC     : DE:AD:BE:EF:55:00
TCP server: port 9000, MAX_CLIENTS=4
RS485   : Serial1, DE_PIN=22, 9600 baud
```

---

## 5. Kiến trúc LOGO! 8 (AC Controller)

### 5.1 Logic điều khiển nội bộ LOGO!

```
V0.4 → XOR → B001 (edge wiping relay, 1s) → Q1 → relay → nút ĐH1 Kế Toán
V0.5 → XOR → B002                          → Q2 → relay → nút ĐH2 P.Nguyên
V0.6 → XOR → B003                          → Q3 → relay → nút ĐH3 P.Họp
V0.7 → XOR → B004                          → Q4 → relay → nút ĐH4 P.Tuấn
```

Feedback:
```
LED bảng ĐH → I1-I4 → M21-M24 → NQ1-NQ4 → VM V0.0-V0.3
```

### 5.2 Modbus TCP address map (LOGO! 8 0BA8)

| Mục đích | Biến LOGO! | Coil Modbus (1-based) | PDU address (0-based) |
|---|---|---|---|
| Điều khiển ĐH1 | V0.4 | Coil 5 | 4 (`LOGO_M_ADDR=4`) |
| Điều khiển ĐH2 | V0.5 | Coil 6 | 5 |
| Điều khiển ĐH3 | V0.6 | Coil 7 | 6 |
| Điều khiển ĐH4 | V0.7 | Coil 8 | 7 |
| Feedback ĐH1 | V0.0 | Coil 1 | 0 (`LOGO_FB_ADDR=0`) |
| Feedback ĐH2 | V0.1 | Coil 2 | 1 |
| Feedback ĐH3 | V0.2 | Coil 3 | 2 |
| Feedback ĐH4 | V0.3 | Coil 4 | 3 |

**Quan trọng**: V0.4-V0.7 là đường điều khiển cho Kramer/API bên ngoài.
M1-M4 (Coil 8257+) là biến nội bộ cho LOGO! Web UI — KHÔNG dùng để ghi từ
Modbus ngoài.

### 5.3 Cách điều khiển qua Modbus (pulse pattern)

```
FC5 Write Coil V0.4 = ON (addr=4, value=0xFF00)
  → chờ 500ms (non-blocking, state machine trong loop())
FC5 Write Coil V0.4 = OFF (addr=4, value=0x0000)
  → LOGO! phát hiện cạnh → B001 tạo xung 1s → Q1 relay nhả → giả lập nhấn nút ĐH
```

LOGO! hỗ trợ tối đa 8 Modbus TCP connections đồng thời. Kramer đang dùng 1 slot.

### 5.4 Lý do dùng iframe thay vì Modbus trực tiếp cho UI

Trang `/logo` trên ESP32 nhúng `http://192.168.1.6/webroot/main.htm` trong
iframe thay vì tự vẽ UI điều khiển LOGO!. Lý do: LOGO! đã có sẵn giao diện
Web Editor đẹp với biểu đồ trạng thái động, tái sử dụng thay vì làm lại.

**Hạn chế đã biết**: Login form trong iframe bị trình duyệt hiện đại chặn
(SameSite cookie policy — cross-origin). Workaround: đăng nhập trên tab riêng
trước, quay lại iframe → nhấn "Tải lại" để dùng session cookie.

---

## 6. Kiến trúc AMX (CE-IO4 + CE-REL8)

### 6.1 Thiết bị

| Thiết bị | IP | Port | Vai trò |
|---|---|---|---|
| CE-IO4 | 192.168.1.7 | 44197 | 4 công tắc tường (digital input) |
| CE-REL8 | 192.168.1.204 | 44197 | 4 relay điều khiển đèn |
| CE-IRS4 | 192.168.1.203 | 44197 | Cảm biến IR (dự phòng, không dùng trong dự án này) |

### 6.2 Giao thức TCP AMX

Định dạng text, suffix `\n` (0x0A):

| Lệnh | Chiều | Mô tả |
|---|---|---|
| `get /io/N/digitalInput` | → CE-IO4 | Đọc trạng thái công tắc N (1-4) |
| `update /io/N/digitalInput true\|false` | CE-IO4 → | Response trạng thái |
| `get /relay/N/state` | → CE-REL8 | Đọc trạng thái relay N (1-4) |
| `update /relay/N/state true\|false` | CE-REL8 → | Response trạng thái |
| `set /relay/N/state true\|false` | → CE-REL8 | Đặt trạng thái relay N |

**Lưu ý**: `Subscribe /io/N/digitalInput` trả lỗi `"unknown path"` trên firmware CE-IO4 thực tế — dùng polling `get` thay thế (mỗi 800ms).

CE-IO4 cần được cấu hình `inputMode = DIGITAL` trước khi đọc được `digitalInput`. Nếu `set /io/N/inputMode DIGITAL` qua TCP không hoạt động → cấu hình thủ công qua web UI `http://192.168.1.7`.

### 6.3 Logic toggle công tắc tường (v2.5.x)

```
CE-IO4 thay đổi trạng thái (bất kể bật→tắt hay tắt→bật)
  → ESP32 ghi nhận: amxPendingToggle[N] = true
                    amxRelayBeforeIO = relay snapshot hiện tại
  → Chờ 500ms (nhường Kramer phản hồi trước)
  → Đọc relay thật từ CE-REL8
  → Nếu relay[N] chưa thay đổi → Kramer chưa xử lý → ESP32 toggle relay[N]
  → Nếu relay[N] đã thay đổi  → Kramer đã xử lý   → ESP32 bỏ qua
```

**Nguyên tắc**: Cả Kramer và ESP32 đều toggle — nhưng chỉ 1 bên thực sự toggle vì bên kia đã làm rồi. Không xung đột, hoạt động song song.

**Web button**: Gọi `set /relay/N/state` trực tiếp → không tạo IO change event → không ảnh hưởng logic toggle.

### 6.4 Kết nối: CE-IO4 persistent, CE-REL8 per-transaction

**CE-IO4 (công tắc tường)**: Persistent connection — `amxIoClient` giữ kết nối liên tục, `amxIoPoll()` đọc non-blocking mỗi vòng `loop()`. Auto-reconnect sau 5s nếu mất kết nối. Lý do: CE-IO4 cần polling 600ms liên tục; mỗi lần connect-per-transaction tích lũy TIME_WAIT → W5500 hết socket.

**CE-REL8 (relay)**: Connect-per-transaction — `amxReadRelays()` và `amxSetRelay()` mở kết nối mới mỗi lần. CE-REL8 chỉ được gọi khi người dùng bấm nút hoặc reconcile, không cần polling liên tục.

---

## 7. Lịch sử lỗi quan trọng & bài học

| Lỗi | Nguyên nhân gốc | Cách sửa | Bài học |
|---|---|---|---|
| Relay button không nhạy (5-8 lần bấm) | Persistent megaClient bị W5500 drop khi nhiều HTTP socket mở/đóng dồn dập | v2.0.0: chuyển sang connect-per-transaction (`megaTransact()`) | W5500 embedded không phù hợp với persistent connection dưới HTTP load cao. Pattern connect-per-transaction tin cậy hơn nhiều |
| Bấm 2 lần nhanh → tự triệt tiêu | `onclick` đọc `classList` (DOM) thay vì state object riêng | `pendingTarget{}` object lưu trạng thái đích độc lập với DOM | Optimistic UI cần state object riêng, không đọc ngược từ DOM |
| Mirror công tắc tường mất hoàn toàn | `waitForBytes()` bắt trúng nhiễu RS485 khi chuyển driver enable | Rollback về `delay(60)` cố định | RS485 half-duplex cần "deadtime" cố định sau khi gửi, không chờ chủ động |
| "TẮT TẤT CẢ" → UI nút vẫn xanh | Nhánh `all off` không cập nhật lạc quan `outSnapshot` ngay | Thêm `outSnapshot = 0` + `mirrorBlockUntil` ngay khi xử lý | Mọi nhánh lệnh đều phải cập nhật snapshot ngay, không chờ sync |
| ESP32 watchdog reset (TG1WDT) | `logoClient.connect()` trong `setup()` block CPU khi LOGO! không phản hồi | Bỏ connect trong `setup()`, chuyển sang connect-per-transaction | Kết nối mạng trong `setup()` nguy hiểm nếu thiết bị đích không phản hồi |
| LOGO! liên tục reconnect/disconnect | LOGO! không giữ idle TCP connection — đóng ngay sau khi không có traffic | Connect-per-transaction thay vì persistent | Thiết bị công nghiệp Siemens thường không giữ idle TCP. Pattern connect-per-transaction phù hợp hơn |
| LOGO! FC5 gửi nhưng output không phản hồi | Ghi vào M1-M4 (Coil 8257+) thay vì V0.4-V0.7 (Coil 5-8) | Đổi `LOGO_M_ADDR` từ 8256 về 4 | Đọc kỹ tài liệu: M1-M4 là đường Web UI nội bộ, V0.4-V0.7 mới là đường Kramer/API |
| HTTP keep-alive: response body rỗng (200 OK nhưng không có data) | Buffer/timing bug trong thư viện Ethernet/W5500 khi tái sử dụng socket | Rollback về Connection: close mỗi request | W5500 thư viện embedded không ổn định với HTTP keep-alive reuse |
| Sáng đến web chết, ping vẫn OK, phải reset ESP32 | Client (browser/kiosk) đóng kết nối trước → socket W5500 kẹt ở `CLOSE_WAIT`. `server.available()` chỉ trả socket có data nên không bao giờ stop() được chúng → tích lũy đến khi cạn cả 8 socket. Ping vẫn OK vì ICMP do phần cứng W5500 xử lý | v3.0.7: `reapDeadSockets()` quét 8 socket mỗi 2s, đóng socket ở `CLOSE_WAIT` (status `0x1C`) không còn data | W5500 không tự thu hồi socket khi remote đóng trước; phải chủ động reap CLOSE_WAIT. `Connection: close` chưa đủ — chỉ xử lý khi server đóng trước |

---

## 8. Quy tắc không làm lại (regressions to avoid)

Những thay đổi đã thử và gây ra vấn đề nghiêm trọng:

- **KHÔNG** dùng `waitForBytes()` thay cho `delay(60)` trong `readIO()` của MEGA — phá vỡ mirror công tắc tường (v1.1.0→v1.5.0).
- **KHÔNG** dùng `anyClientPending()` để gate polling I/O trên MEGA — khiến mirror công tắc tường không bao giờ chạy (v1.2.0→v1.4.0).
- **KHÔNG** dùng HTTP keep-alive trên W5500 — response body rỗng dù header đúng (v1.5.0→v1.6.0).
- **KHÔNG** chờ đồng bộ (blocking wait) trong route `/cmd` trước khi trả response — gây cộng hưởng với chu kỳ IO polling của MEGA (v1.2.0→v1.3.0).
- **KHÔNG** giữ persistent TCP connection tới MEGA hoặc LOGO! — W5500 drop dưới tải HTTP cao. Luôn dùng connect-per-transaction (v2.0.0).
- **KHÔNG** kết nối mạng (`client.connect()`) trong `setup()` — gây Watchdog reset nếu thiết bị đích không phản hồi (v1.8.0→v1.8.1).

---

## 9. Cấu trúc file & version hiện tại

```
Code_ESP32_Web_UI_TCP_client_.ino              v3.0.1   ESP32 Web server + SPA gateway
Code_ArduinoMEGA2560_W5500_TCP_Max485_.ino     v1.5.0   MEGA RS485 controller
DESIGN.md                                               Tài liệu kiến trúc (file này)
Changelog.md                                            Lịch sử thay đổi tất cả firmware
README.md                                               Hướng dẫn cài đặt & vận hành
HuongDanSuDung.doc                                      Hướng dẫn sử dụng người dùng cuối
```

### Thư viện cần thiết

| Board | Thư viện |
|---|---|
| ESP32 | `Ethernet` (chuẩn), `Preferences` (NVS — có sẵn trong ESP32 Arduino core) |
| MEGA2560 | `Ethernet2` (cho W5500) |

---

## 10. Tình trạng phát triển hiện tại

### Đã hoàn thiện ✅
- 16 relay điều khiển đèn qua UI web + công tắc vật lý + scene switch (MEGA) — v1.5.0
- Nút bấm dồn dập nhanh hoạt động đúng, connect-per-transaction — v2.0.0
- "TẮT TẤT CẢ" đồng bộ UI đúng — v1.3.0 (MEGA)
- Điều khiển 4 điều hòa qua LOGO! 8 Modbus TCP (pulse pattern) — v1.8.x / v2.x
- AMX CE-REL8 4 relay + CE-IO4 4 công tắc tường, polling 600ms — v2.3.0+
- AMX toggle logic: IO thay đổi → toggle relay (không map 1-1 trạng thái) — v2.4.0
- AMX Kramer co-existence: ESP32 nhường 300ms, chỉ toggle nếu Kramer chưa làm — v2.4.0
- CE-IO4 persistent connection, tránh TIME_WAIT, không còn ERR_CONNECTION_REFUSED — v2.5.0
- Relay không nháy khi ESP32 khởi động (seed IO snapshot trước khi bật toggle) — v2.5.0
- Tên nút relay AMX: P.Họp / P.Tuấn / K.Doanh / H.Lang — v2.4.0
- SPA: không load lại trang khi chuyển tab, không còn mất giao diện trên kiosk — v2.6.0
- KIOS iframe browser với thanh địa chỉ, Tải lại, Mở tab mới — v2.6.0
- Xác thực cookie-based (mật khẩu duy nhất, form HTML), /login + /logout — v2.7.1
- Giảm Serial log: bỏ spam MEGA/AMX connect fail, chỉ log IO khi có thay đổi — v2.8.0
- NTP đồng bộ giờ thực (216.239.35.0, UDP port 123, UTC+7), đồng hồ trên nav bar — v2.9.0
- Lịch tắt tự động theo giờ hằng ngày (schedEntry), bù boot 5 phút — v2.9.0
- /info endpoint JSON: firmware version, uptime, giờ hệ thống, trạng thái NTP, lịch — v3.0.0
- Tab Cài đặt (Settings): toggle lịch, chỉnh giờ tắt, nút lưu, card thông tin hệ thống — v3.0.0
- Lưu cấu hình lịch vào NVS (Preferences) — tồn tại sau khởi động lại — v3.0.1
- Đổi mật khẩu qua Settings tab, lưu NVS (key `auth_p`), xác nhận mật khẩu cũ — v3.0.2
- Fix: lịch/giờ không lưu được do `loadSched()` ghi đè form mỗi 2s — v3.0.2
- Form đổi mật khẩu ẩn sau easter egg: nhấn tiêu đề "Thông tin hệ thống" 5 lần — v3.0.5
- Reap socket CLOSE_WAIT chống cạn 8 socket W5500 (web chết, ping OK) — v3.0.7
- Khối cài đặt ẩn (Hẹn giờ tắt + Hiển thị tab + Đổi mật khẩu) sau easter egg; ẩn/hiện tab nav lưu NVS — v3.0.8
- Nút Đăng xuất trên nav bar — v3.0.0

### Còn hạn chế / chưa xác nhận 🔲
- CE-IO4: `Subscribe` path không hoạt động (firmware CE-IO4 thực tế) → polling `get` mỗi 600ms (độ trễ tối đa 600ms)
- CE-IO4: `set /io/N/inputMode DIGITAL` qua TCP chưa xác nhận hoàn toàn — nếu lỗi cần cấu hình thủ công tại `http://192.168.1.7`
- Timeout client TCP trên MEGA (`CLIENT_TIMEOUT = 10000` định nghĩa nhưng chưa dùng)
- Xác thực CRC cho RS485 frame

---

## 11. Thông tin dự án

| | |
|---|---|
| **Tên** | TNN Smart Control |
| **Chủ đầu tư** | TNN SI |
| **Website** | www.tnnsi.vn |
| **Ngôn ngữ** | C++ (Arduino framework) |
| **IDE** | Arduino IDE |
| **Mạng** | LAN nội bộ, IP tĩnh |
| **Truy cập web** | `http://192.168.1.180` |
| **LOGO! web** | `http://192.168.1.6` — User: `Web User`, Pass: `123456` |
