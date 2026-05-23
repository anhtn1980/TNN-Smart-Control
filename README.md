# TNN Smart Control

Hệ thống điều khiển relay qua mạng LAN gồm 2 tầng:

- **ESP32 + W5500**: cung cấp Web UI/API cho người dùng và chuyển lệnh TCP.
- **Arduino MEGA2560 + W5500 + MAX485/RS485**: nhận lệnh TCP, điều khiển relay qua RS485, đồng bộ trạng thái I/O.

---

## 1) Kiến trúc tổng quan

```text
[Điện thoại/PC]
      |
   HTTP (Web UI/API)
      |
[ESP32 + W5500]  --- TCP:9000 ---> [MEGA2560 + W5500 + MAX485] --- RS485 ---> [Board relay 16 kênh]
```

### Vai trò từng thiết bị

- **ESP32**
  - Chạy web server cổng `80`.
  - Cung cấp giao diện điều khiển relay 1–16.
  - Gửi lệnh TCP dạng text sang MEGA (`set /relay/x/toggle`, `set /system/all off`, `get /relay/all`).
  - Nhận trạng thái relay từ MEGA để cập nhật UI.

- **MEGA2560**
  - Chạy TCP server cổng `9000`.
  - Parse lệnh text và gửi frame RS485 tương ứng.
  - Đọc trạng thái input/output định kỳ từ module RS485.
  - Hỗ trợ scene switch bằng nút cứng (`SCENE_PIN`).

---

## 2) Cấu trúc file chính

- `Code_ESP32_Web_UI_TCP_client_.ino`
  - Firmware cho ESP32 (Web UI + TCP client).

- `Code_ArduinoMEGA2560_W5500_TCP_Max485_.ino`
  - Firmware cho Arduino MEGA2560 (TCP server + RS485 control).

---

## 3) Thông số mạng mặc định

### ESP32

- IP tĩnh: `192.168.1.180`
- Gateway: `192.168.1.1`
- Subnet: `255.255.255.0`
- DNS: `8.8.8.8`
- Web server: port `80`
- TCP client target: `192.168.1.178:9000`

### MEGA2560

- IP tĩnh: `192.168.1.178`
- Gateway: `192.168.1.1`
- Subnet: `255.255.255.0`
- TCP server: port `9000`

> Điều chỉnh dải IP cho phù hợp router/mạng LAN thực tế trước khi triển khai.

---

## 4) API và protocol lệnh

### HTTP endpoint trên ESP32

- `GET /`
  - Trả về Web UI điều khiển.

- `GET /cmd?c=<command>`
  - Gửi trực tiếp chuỗi lệnh sang MEGA qua TCP.
  - Ví dụ:
    - `/cmd?c=set%20/relay/1/toggle`
    - `/cmd?c=set%20/system/all%20off`

- `GET /set?relay=<n>&value=<true|false|1|0>`
  - Đặt trạng thái relay theo số kênh.
  - Ví dụ: `/set?relay=3&value=true`

- `GET /status`
  - Trả text trạng thái dạng: `L1=0,L2=1,...,L16=0`

- `GET /api/status`
  - Trả JSON: `{"raw":"L1=0,..."}`

### Lệnh TCP text mà MEGA hỗ trợ

- `set /relay/<1..16>/toggle`
- `set /relay/<1..16>/state true|false`
- `set /system/all off`
- `get /relay/all`

---

## 5) Luồng hoạt động

1. Người dùng mở web tại `http://192.168.1.180/`.
2. Nhấn nút relay trên UI.
3. ESP32 gửi lệnh TCP sang MEGA.
4. MEGA gửi frame RS485 để toggle relay tương ứng.
5. MEGA cập nhật snapshot I/O, trả trạng thái về client TCP.
6. ESP32 polling `/status` để hiển thị trạng thái thực tế trên UI.

---

## 6) Hướng dẫn nạp firmware

## Yêu cầu phần cứng

- 01 x ESP32 + module W5500
- 01 x Arduino MEGA2560 + module W5500
- 01 x MAX485 (hoặc transceiver RS485 tương đương)
- Board relay RS485 16 kênh tương thích frame điều khiển hiện có
- Nguồn cấp phù hợp cho tất cả thiết bị

## Thư viện Arduino

- ESP32 sketch:
  - `SPI`
  - `Ethernet`

- MEGA sketch:
  - `SPI`
  - `Ethernet2`

> Cài đúng thư viện Ethernet tương ứng từng sketch để tránh xung đột.

## Các bước

1. Mở `Code_ArduinoMEGA2560_W5500_TCP_Max485_.ino` trong Arduino IDE.
2. Chọn board **Arduino Mega or Mega 2560** và đúng COM port.
3. Build & Upload cho MEGA.
4. Mở `Code_ESP32_Web_UI_TCP_client_.ino`.
5. Chọn board ESP32 phù hợp và COM port.
6. Build & Upload cho ESP32.
7. Kết nối dây mạng LAN cho cả ESP32 và MEGA.
8. Truy cập `http://192.168.1.180` để kiểm tra.

---

## 7) Mapping chân (theo code hiện tại)

## ESP32 (W5500)

- `W5500_CS = 5`
- `W5500_RST = 4`
- `SPI_SCK = 18`
- `SPI_MISO = 19`
- `SPI_MOSI = 23`

## MEGA2560

- `DE_PIN = 22` (điều khiển TX/RX MAX485)
- `RS485 = Serial1`
- `SCENE_PIN = 30` (nút scene, `INPUT_PULLUP`)

---

## 8) Lưu ý vận hành

- ESP32 có cơ chế reconnect TCP tới MEGA mỗi ~3 giây khi mất kết nối.
- Cơ chế `mirrorBlockUntil` trên MEGA giúp tránh phản hồi chéo ngay sau thao tác relay.
- Nút **AC** trên UI ESP32 đang gửi lệnh `set /ac/x/pulse`; nếu MEGA chưa triển khai parser `/ac/` thì lệnh này sẽ không có tác dụng.

---

## 9) Gợi ý cải tiến tiếp theo

- Chuẩn hóa parser HTTP/URL decode đầy đủ trên ESP32.
- Bổ sung xác thực CRC frame RS485 khi đọc response.
- Triển khai timeout client TCP (`CLIENT_TIMEOUT`) đầy đủ trên MEGA.
- Bổ sung logging có cấu trúc để debug nhanh trong thực địa.
- Tách protocol command sang module riêng để dễ mở rộng.

---

## 10) Bản quyền & liên hệ

Tài liệu này được tạo để phục vụ vận hành nội bộ dự án **TNN Smart Control**.
Bạn có thể bổ sung thông tin tác giả/đơn vị vận hành tại mục này.
