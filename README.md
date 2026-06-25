# TNN Smart Control

Hệ thống điều khiển đèn và điều hòa không khí qua mạng LAN nội bộ, hỗ trợ PC và điện thoại, không phụ thuộc WiFi.

---

## 1. Kiến trúc tổng quan

```
[PC / Điện thoại / Kiosk]
           │
        HTTP :80
           │
   [ESP32 + W5500]  192.168.1.180  ← Web server, SPA UI, HTTP API gateway
     │        │         │               │
  TCP:9000  Modbus   TCP:44197       TCP:44197
     │      TCP:504      │               │
[MEGA2560] [LOGO!8]  [CE-IO4]       [CE-REL8]
192.168.1.178  .6      .7             .204
     │
  RS485 9600
     │
[Board relay 16 kênh]
```

| Thiết bị | IP | Vai trò |
|---|---|---|
| ESP32 + W5500 | 192.168.1.180 | Web server port 80, SPA UI (v2.6.0) |
| MEGA2560 + W5500 | 192.168.1.178 | TCP server :9000, 16 relay RS485 (v1.5.0) |
| LOGO! 8 0BA8 | 192.168.1.6 | Modbus TCP :504, 4 điều hòa |
| AMX CE-IO4 | 192.168.1.7 | TCP :44197, 4 công tắc tường (input) |
| AMX CE-REL8 | 192.168.1.204 | TCP :44197, 4 relay đèn (output) |
| AMX CE-IRS4 | 192.168.1.203 | TCP :44197, cảm biến IR (dự phòng) |

---

## 2. Giao diện web

Truy cập `http://192.168.1.180` từ bất kỳ thiết bị nào trên LAN.

Giao diện SPA (Single Page Application) gồm 4 tab, chuyển tab không cần tải lại trang:

| Tab | Nội dung |
|---|---|
| **Đèn** | 16 relay đèn (MEGA2560), nút TẮT TẤT CẢ |
| **Điều hòa** | 4 điều hòa (LOGO! 8), nút bật/tắt từng máy |
| **AMX** | 4 relay đèn CE-REL8 + trạng thái 4 công tắc tường CE-IO4 |
| **KIOS** | Iframe nhúng KC-Brain, SL-240C hoặc URL tùy chọn |

---

## 3. Cấu hình mạng

### ESP32 (Web server)
- IP tĩnh: `192.168.1.180`
- Gateway: `192.168.1.1` — Subnet: `255.255.255.0`
- Web server: port `80`

### MEGA2560 (RS485 Controller)
- IP tĩnh: `192.168.1.178`
- TCP server: port `9000`

### LOGO! 8 (Điều hòa)
- IP tĩnh: `192.168.1.6`
- Modbus TCP: port `504`
- Web UI: `http://192.168.1.6/webroot/main.htm` — User: `Web User`, Pass: `123456`

---

## 4. HTTP API

| Endpoint | Mô tả |
|---|---|
| `GET /` | Trang SPA chính (HTML đầy đủ) |
| `GET /cmd?c=<lệnh>` | Gửi lệnh text tới MEGA (relay, all-off) |
| `GET /status` | Trạng thái 16 relay: `L1=0,L2=1,...,L16=0` |
| `GET /ac/toggle?ch=N` | Toggle điều hòa N (1-4) |
| `GET /ac/status` | JSON trạng thái 4 điều hòa |
| `GET /amx/relay?ch=N&value=true\|false` | Đặt relay CE-REL8 kênh N |
| `GET /amx/status` | JSON: `{"relay":[...],"io":[...]}` CE-REL8 + CE-IO4 |

### Lệnh MEGA (`/cmd?c=<lệnh>`)
- `set /relay/<1-16>/state true|false`
- `set /relay/<1-16>/toggle`
- `set /system/all off`

---

## 5. Thư viện Arduino

| Board | Thư viện |
|---|---|
| ESP32 | `Ethernet` (chuẩn, không dùng Ethernet2) |
| MEGA2560 | `Ethernet2` (cho W5500) |

---

## 6. Nạp firmware

1. Mở `Code_ArduinoMEGA2560_W5500_TCP_Max485_.ino` trong Arduino IDE.
2. Chọn board **Arduino Mega or Mega 2560**, đúng COM port → Upload.
3. Mở `Code_ESP32_Web_UI_TCP_client_.ino`.
4. Chọn board ESP32 phù hợp, đúng COM port → Upload.
5. Kết nối dây LAN cho cả 2 board.
6. Truy cập `http://192.168.1.180` để kiểm tra.

---

## 7. Mapping chân

### ESP32 (W5500)
| Tín hiệu | GPIO |
|---|---|
| W5500 CS | 5 |
| W5500 RST | 4 |
| SPI SCK | 18 |
| SPI MISO | 19 |
| SPI MOSI | 23 |

### MEGA2560
| Tín hiệu | Pin |
|---|---|
| RS485 DE/RE | 22 |
| RS485 Serial | Serial1 |
| Scene switch | 30 (INPUT_PULLUP) |

---

## 8. Lưu ý vận hành

- **W5500 chỉ có 8 socket phần cứng** — giao diện SPA (v2.6.0) tránh mở socket mới khi chuyển tab.
- **CE-IO4 persistent connection** — tránh tích lũy TIME_WAIT, không còn ERR_CONNECTION_REFUSED.
- **Relay không nháy khi ESP32 boot** — firmware đọc trạng thái thực tế công tắc tường trước khi bật toggle logic.
- **LOGO! không giữ idle TCP** — ESP32 dùng connect-per-transaction (mở→gửi→đóng mỗi lần).
- **AMX toggle logic**: bấm công tắc tường → toggle relay, không map 1:1 trạng thái. Kramer và ESP32 cùng listen nhưng chỉ 1 bên xử lý (ESP32 nhường 300ms).
- **Scene switch MEGA** (nút cứng pin 30): có đèn đang bật → tắt hết; tất cả tắt → bật kênh 12 (Kế Toán).

---

## 9. Lịch sử version

| Version | Mô tả |
|---|---|
| ESP32 v2.6.0 | SPA refactor — không reload trang, giải quyết ERR_CONNECTION_REFUSED trên kiosk |
| ESP32 v2.5.0 | CE-IO4 persistent connection, seed IO snapshot, relay không nháy khi boot |
| ESP32 v2.4.0 | AMX toggle logic, Kramer co-existence, tên relay P.Họp/P.Tuấn/K.Doanh/H.Lang |
| ESP32 v2.3.0 | CE-IO4 polling `get`, fix IP 192.168.1.7 |
| ESP32 v2.0.0 | Connect-per-transaction cho MEGA, giải quyết RECONNECTED OK loop |
| MEGA v1.5.0 | Rollback `waitForBytes()` → `delay(60)` cố định, mirror công tắc vật lý hoạt động đúng |

Xem `Changelog.md` để biết chi tiết từng thay đổi.

---

## 10. Tài liệu liên quan

- `DESIGN.md` — Kiến trúc chi tiết, quyết định thiết kế, lịch sử lỗi
- `Changelog.md` — Log thay đổi đầy đủ từng version
- `HuongDanSuDung.doc` — Hướng dẫn sử dụng cho người dùng cuối

---

## 11. Bản quyền & liên hệ

Tài liệu phục vụ vận hành nội bộ dự án **TNN Smart Control — TNN SI**.
Website: [www.tnnsi.vn](http://www.tnnsi.vn)
