# CLAUDE.md — TNN Smart Control

Hướng dẫn làm việc cho Claude Code trong project này.

---

## Tổng quan nhanh

Hệ thống điều khiển đèn + điều hòa văn phòng qua LAN nội bộ.
- **ESP32 + W5500** (`192.168.1.180`): Web server port 80, SPA UI, HTTP API gateway — firmware `Code_ESP32_Web_UI_TCP_client_.ino` v3.0.7
- **MEGA2560 + W5500** (`192.168.1.178`): TCP server port 9000, 16 relay RS485 — firmware `Code_ArduinoMEGA2560_W5500_TCP_Max485_.ino` v1.5.0
- **LOGO! 8** (`192.168.1.6:504`): Modbus TCP, 4 điều hòa
- **AMX CE-IO4** (`192.168.1.7:44197`): 4 công tắc tường (input)
- **AMX CE-REL8** (`192.168.1.204:44197`): 4 relay đèn (output)

Chi tiết đầy đủ: xem `DESIGN.md`.

---

## Ràng buộc phần cứng — ĐỌC TRƯỚC KHI SỬA

### W5500 chỉ có 8 socket phần cứng
Đây là ràng buộc quan trọng nhất. Mọi quyết định kiến trúc đều bị chi phối bởi điều này.
- Giao diện web là **SPA** (Single Page Application) — 1 HTTP response duy nhất, JS chuyển tab không reload trang → không tốn socket mới mỗi lần chuyển tab.
- CE-IO4 dùng **persistent connection** (`amxIoClient`) — không đóng/mở lại để tránh tích lũy TIME_WAIT.
- MEGA và LOGO! dùng **connect-per-transaction** — mở→gửi→đóng mỗi lần, vì chúng không giữ idle TCP.
- **Socket reaper** (`reapDeadSockets()`, v3.0.7) — quét 8 socket mỗi 2s, đóng socket kẹt `CLOSE_WAIT` (status `0x1C`). Bắt buộc: client đóng kết nối trước → socket kẹt CLOSE_WAIT → `server.available()` không trả về → cạn dần 8 socket → web chết nhưng ping OK. KHÔNG xóa hàm này.

### Những thứ KHÔNG làm lại
- **KHÔNG** dùng HTTP keep-alive trên W5500 — response body rỗng dù header đúng.
- **KHÔNG** kết nối mạng (`client.connect()`) trong `setup()` — gây Watchdog reset nếu thiết bị đích offline.
- **KHÔNG** dùng `waitForBytes()` thay `delay(60)` trong MEGA `readIO()` — phá vỡ mirror công tắc tường.
- **KHÔNG** giữ persistent TCP tới MEGA hoặc LOGO! — W5500 drop dưới tải HTTP cao.

---

## Quy tắc khi sửa firmware ESP32

1. **Tăng `FW_VERSION`** mỗi khi có thay đổi — định nghĩa ở đầu file `#define FW_VERSION "x.y.z"`.
2. **Không dùng blocking wait** trong `loop()` — mọi thứ phải non-blocking.
3. **Không thêm Serial.print** trong các hàm gọi thường xuyên (polling, AMX read) — Serial log sẽ flood và không quan sát được.
4. **Không mở socket mới** khi chuyển tab — vi phạm ràng buộc W5500.
5. Sau khi sửa HTML/JS trong SPA: nhớ rằng toàn bộ UI là chuỗi `client.println(...)` trong C++ — không có file HTML riêng.

---

## Quy tắc khi sửa firmware MEGA

1. Không thay đổi cơ chế `delay(60)` trong `readIO()` — xem mục "Những thứ KHÔNG làm lại".
2. Giao thức RS485 là Modbus-like tự chế, không phải Modbus chuẩn — không dùng thư viện Modbus generic.

---

## Cách test

Không có unit test. Test thực tế:
- **Serial Monitor** (115200 baud) — quan sát log boot, NTP sync, IO state change.
- **Trình duyệt** `http://192.168.1.180` — kiểm tra UI trực tiếp.
- **curl/browser** các endpoint: `/status`, `/ac/status`, `/amx/status`, `/info`.
- Xác nhận `FW_VERSION` mới hiển thị đúng tại tab Cài đặt → thông tin hệ thống.

---

## Xác thực

- Cookie-based session, password only (không có username).
- Biến `authPass` đọc từ NVS khi boot (namespace `"tnn"`, key `auth_p"`, default `"123456"`).
- Token `authToken` tạo mới mỗi lần reboot — mọi session cũ tự hết hạn sau reboot.
- Form đổi mật khẩu ẩn trong tab Cài đặt — nhấn tiêu đề "Thông tin hệ thống" 5 lần liên tiếp.
- Form cấu hình hiển thị tab ẩn trong tab Cài đặt — nhấn tiêu đề "Thông tin hệ thống" 3 lần liên tiếp.

---

## NVS (Preferences)

Namespace `"tnn"`, các key hiện tại:

| Key | Type | Mặc định | Ý nghĩa |
|---|---|---|---|
| `sched_en` | bool | true | Lịch tắt bật/tắt |
| `sched_h` | uint8 | 18 | Giờ tắt |
| `sched_m` | uint8 | 0 | Phút tắt |
| `auth_p` | String | "123456" | Mật khẩu đăng nhập |
| `tab_vis` | uint8 | 0x1F | Bitmask hiển thị tab nav (bit0=Đèn…bit4=Cài đặt; bit4 luôn=1) |

---

## NTP

- Server: `216.239.35.0` (time.google.com, IP cố định — không dùng DNS vì W5500 thư viện Ethernet chuẩn không có DNS resolver tốt).
- Múi giờ: UTC+7 (cộng 25200 giây).
- Đồng bộ lại mỗi 6 giờ. Retry mỗi 30 giây nếu chưa sync.
- `lastNtpAttempt = millis()` phải được set sau `ntpSync()` trong `setup()` — tránh loop() retry ngay lập tức.

---

## Scheduler

- Biến: `schedEntry {hour, minute}`, `schedEnabled`, `schedFiredToday` (bitmask ngày).
- Khi đến giờ: tắt 16 relay MEGA + 4 điều hòa LOGO! + 4 relay CE-REL8.
- Boot window 5 phút: nếu reboot xảy ra trong vòng 5 phút sau giờ hẹn thì vẫn thực hiện tắt.
- Reset `schedFiredToday = 0` mỗi khi lưu giờ mới — cho phép áp dụng ngay trong ngày.

---

## AMX CE-IO4

- Dùng **polling** `get /io/N/digitalInput` mỗi 600ms (Subscribe không hoạt động trên firmware thực tế).
- **Persistent connection** `amxIoClient` — reconnect tự động nếu mất kết nối.
- Khi kết nối, ESP32 tự gửi `set /io/N/mode INPUT` + `set /io/N/inputMode DIGITAL` cho 4 IO — người dùng không cần cấu hình thủ công trừ khi lệnh set bị firmware CE-IO4 từ chối.
- Toggle logic: IO thay đổi → đợi 300ms nhường Kramer → đọc relay thật → toggle nếu Kramer chưa làm.

---

## Git

- Branch làm việc: xem system prompt của session hiện tại.
- Không push vào `main` trực tiếp.
- Commit message: `feat`/`fix`/`docs` + mô tả ngắn + version nếu có.
- Sau mỗi nhóm thay đổi lớn: cập nhật `Changelog.md` và `DESIGN.md`.
