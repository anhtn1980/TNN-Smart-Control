# TNN Smart Control — Tài liệu thiết kế tổng thể

> **Mục đích**: Tài liệu "sống" phục vụ đồng thời cho tác giả và Claude
> trong các phiên làm việc tiếp theo. Cập nhật mỗi khi có thay đổi kiến trúc,
> quyết định thiết kế quan trọng, hoặc lỗi đã sửa cần ghi nhớ lý do.
>
> Lần cập nhật gần nhất: v2.0.0 — 2026-06-24

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
       │                   │
    TCP :9000         Modbus TCP :502
       │                   │
[MEGA2560 + W5500]   [LOGO! 8]  192.168.1.6
  192.168.1.178
       │
    RS485 9600
       │
[Board relay 16 kênh]
```

### Vai trò từng thiết bị

| Thiết bị | Vai trò | Firmware |
|---|---|---|
| ESP32 + W5500 | Web server port 80, HTTP API, giao diện web | v2.0.0 |
| MEGA2560 + W5500 | TCP server port 9000, điều khiển relay qua RS485 | v1.5.0 |
| LOGO! 8 | Modbus TCP server port 502, điều khiển 4 điều hòa | — (Siemens) |

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

| Route | Mô tả |
|---|---|
| `GET /` | Menu chính — 4 nút điều hướng |
| `GET /mega` | Trang 16 relay (đèn) |
| `GET /logo` | Iframe nhúng giao diện LOGO! Web Editor (192.168.1.6/webroot/main.htm) |
| `GET /amx` | Placeholder — phát triển sau |
| `GET /modbus` | Placeholder — phát triển sau |
| `GET /cmd?c=<lệnh>` | Gửi lệnh tới MEGA qua `megaTransact()` |
| `GET /status` | Trả cached `globalStatus` (L1=0,...,L16=0) |
| `GET /set?relay=N&value=true\|false` | API đặt trạng thái relay |
| `GET /api/status` | JSON: `{"raw":"L1=0,..."}` |

### 3.3 JavaScript UI (trang /mega)

**Vấn đề**: Bấm 2 lần nhanh liên tiếp → 2 lần tự triệt tiêu (v1.3.0).

**Nguyên nhân**: `onclick` đọc trực tiếp `classList.contains('on')` mỗi lần
bấm. Lần 2 đọc màu vừa bị lần 1 đổi, đảo ngược lại, net effect = không làm gì.

**Giải pháp**: `pendingTarget{}` — object lưu trạng thái đích dự kiến riêng
cho từng nút. Mỗi lần bấm: đọc `pendingTarget[key]` (không đọc DOM), đảo,
ghi lại, gửi lệnh. `applyStatus()` đồng bộ `pendingTarget` theo trạng thái
thật khi nhận data từ server.

### 3.4 Cấu hình mạng ESP32

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

## 6. Lịch sử lỗi quan trọng & bài học

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

---

## 7. Quy tắc không làm lại (regressions to avoid)

Những thay đổi đã thử và gây ra vấn đề nghiêm trọng:

- **KHÔNG** dùng `waitForBytes()` thay cho `delay(60)` trong `readIO()` của MEGA — phá vỡ mirror công tắc tường (v1.1.0→v1.5.0).
- **KHÔNG** dùng `anyClientPending()` để gate polling I/O trên MEGA — khiến mirror công tắc tường không bao giờ chạy (v1.2.0→v1.4.0).
- **KHÔNG** dùng HTTP keep-alive trên W5500 — response body rỗng dù header đúng (v1.5.0→v1.6.0).
- **KHÔNG** chờ đồng bộ (blocking wait) trong route `/cmd` trước khi trả response — gây cộng hưởng với chu kỳ IO polling của MEGA (v1.2.0→v1.3.0).
- **KHÔNG** giữ persistent TCP connection tới MEGA hoặc LOGO! — W5500 drop dưới tải HTTP cao. Luôn dùng connect-per-transaction (v2.0.0).
- **KHÔNG** kết nối mạng (`client.connect()`) trong `setup()` — gây Watchdog reset nếu thiết bị đích không phản hồi (v1.8.0→v1.8.1).

---

## 8. Cấu trúc file & version hiện tại

```
Code_ESP32_Web_UI_TCP_client_.ino    v2.0.0   ESP32 Web server + gateway
Code_ArduinoMEGA2560_W5500_TCP_Max485_.ino  v1.5.0   MEGA RS485 controller
CHANGELOG.md                                  Lịch sử thay đổi theo version
DESIGN.md                                     File này
```

### Thư viện cần thiết

| Board | Thư viện |
|---|---|
| ESP32 | `Ethernet` (chuẩn) |
| MEGA2560 | `Ethernet2` (cho W5500) |

---

## 9. Tình trạng phát triển hiện tại

### Đã hoàn thiện ✅
- 16 relay điều khiển đèn qua UI web + công tắc vật lý + scene switch
- Nút bấm dồn dập nhanh hoạt động đúng (v2.0.0)
- "TẮT TẤT CẢ" đồng bộ UI đúng
- Menu 4 trang: /mega /logo /amx /modbus
- /logo: iframe LOGO! Web Editor với fallback + reload

### Chưa phát triển 🔲
- **Trang /modbus**: điều khiển điều hòa qua Modbus TCP trực tiếp từ ESP32 UI
  - Backend đã có: `logoTransact()`, `logoFC5()`, `logoSendPulse()`
  - Cần: trang UI với 4 nút + feedback trạng thái thật từ V0.0-V0.3
- **Trang /amx**: điều khiển thiết bị AMX
- Timeout client TCP trên MEGA (`CLIENT_TIMEOUT = 10000` định nghĩa nhưng chưa dùng)
- Xác thực CRC cho RS485 frame

---

## 10. Thông tin dự án

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
