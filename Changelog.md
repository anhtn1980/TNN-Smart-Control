# CHANGELOG — TNN Smart Control

Quy ước: mỗi file `.ino` có số version riêng (Semantic Versioning: MAJOR.MINOR.PATCH), chỉ tăng khi chính file đó được sửa. Mọi thay đổi của cả 2 firmware được ghi chung vào file này theo thứ tự thời gian để dễ theo dõi.

---

## Code_ArduinoMEGA2560_W5500_TCP_Max485_.ino

### [1.5.0] - 2026-06-20
Rollback `waitForBytes()` (thêm ở v1.1.0) — đây là nguyên nhân thật sự khiến mirror công tắc tường mất hoàn toàn (rollback `anyClientPending()` ở v1.4.0 không đủ, vì đó không phải nguyên nhân chính). Người dùng xác nhận công tắc tường hoạt động đúng trên code gốc (trước mọi thay đổi), và hoàn toàn không phản hồi sau khi nâng cấp — trong khi lệnh ghi relay qua UI vẫn hoạt động bình thường suốt các bản trước đó.

Phân tích: RS485 là bus bán song công, cần thời gian ổn định sau khi chuyển driver enable (DE_PIN) trước khi dữ liệu phản hồi đáng tin cậy. Bản gốc luôn đợi đủ `delay(60)` trước khi đọc. `waitForBytes()` ở v1.1.0 kiểm tra `RS485.available()` gần như ngay lập tức sau khi gửi lệnh đọc, nhiều khả năng bắt trúng nhiễu thoáng qua trên bus đúng lúc chuyển trạng thái driver — vì code không có xác thực CRC để loại dữ liệu rác, các byte "đủ số lượng" đó được xử lý như phản hồi thật nhưng giá trị sai/không ổn định. Vì input đọc cho mirror cần 2 lần đọc liên tiếp khớp nhau mới kích hoạt (debounce), dữ liệu không ổn định khiến điều kiện này không bao giờ thỏa — mirror coi như chết hẳn. Lệnh ghi relay (`rs485Send` đơn thuần, fire-and-forget, không đọc lại) không đi qua đường này nên không bị ảnh hưởng, giải thích vì sao UI vẫn hoạt động bình thường suốt thời gian mirror bị hỏng.
- `readIO()`: bỏ `waitForBytes()`, quay lại `delay(60)` cố định như bản gốc cho cả 2 lần đọc (input/output).
- Xóa macro `RS485_READ_TIMEOUT_MS` (không còn dùng).
- `IO_POLL_INTERVAL_MS` (gating tần suất gọi `readIO()` trong `loop()`, không liên quan tới cơ chế đọc dữ liệu) được giữ nguyên — không phải nguyên nhân gây lỗi.

### [1.4.0] - 2026-06-20
Rollback `anyClientPending()` (thêm ở v1.2.0) — sau khi test thực tế, thay đổi này làm **mất hẳn logic mirror công tắc vật lý gắn tường**. Nguyên nhân nghi ngờ: cơ chế gate "bỏ qua lượt polling I/O nếu đang có lệnh TCP chờ xử lý" có thể bị giữ đúng (luôn thấy có dữ liệu chờ) trong điều kiện thực tế, khiến vòng polling — nơi duy nhất phát hiện thay đổi từ input vật lý — gần như không bao giờ chạy. Vì lợi ích của thay đổi này (giảm độ trễ khi nhịp bấm trùng chu kỳ polling) chưa kịp xác nhận rõ ràng trong khi rủi ro gây mất hẳn 1 tính năng quan trọng là có thật, quyết định rollback hoàn toàn về logic polling thuần của v1.1.0 thay vì vá thêm.
- Xóa hàm `anyClientPending()`.
- `loop()`: bỏ điều kiện `!anyClientPending()` khỏi gate polling I/O, quay về đúng logic v1.1.0 (chỉ gate theo `IO_POLL_INTERVAL_MS`).

(Lưu ý: sau khi nạp v1.4.0, mirror công tắc tường vẫn chưa hoạt động — nguyên nhân thật sự được xác định ở v1.5.0 là `waitForBytes()`, không phải `anyClientPending()`. Mục này giữ lại để truy vết lịch sử thay đổi.)

### [1.3.0] - 2026-06-20
Sửa lỗi: bấm "TẮT TẤT CẢ" → đèn tắt thật nhưng nút trên UI vẫn hiện xanh (ON) vĩnh viễn cho tới khi bấm nút khác. Nguyên nhân: nhánh `set /system/all off` trong `execCommand()` không cập nhật lạc quan `outSnapshot` ngay lập tức như nhánh `/relay/.../state` vẫn làm — phản hồi trạng thái gửi về ESP32 ngay sau lệnh vẫn còn các bit ON cũ; lần đọc lại đúng 150ms sau đó chỉ sửa `outSnapshot` nội bộ trên MEGA mà không có cơ chế nào chủ động đẩy trạng thái mới đó qua lại cho client TCP.
- Thêm `outSnapshot = 0` và `mirrorBlockUntil` ngay khi xử lý `set /system/all off`, khớp với cách các lệnh relay riêng lẻ đã làm.

### [1.2.0] - 2026-06-20 (ROLLBACK — xem v1.4.0)
~~Giảm cộng hưởng giữa nhịp bấm nút dồn dập và chu kỳ polling I/O bằng cách nhường ưu tiên cho lệnh TCP, bỏ qua lượt polling nếu có lệnh đang chờ.~~ Đã rollback ở v1.4.0 do làm mất logic mirror công tắc vật lý. Giữ lại đoạn này để truy vết lịch sử thay đổi.

### [1.1.0] - 2026-06-18
Cải thiện độ nhạy xử lý lệnh relay từ mạng (khắc phục hiện tượng phải bấm nhiều lần mới thấy relay phản hồi):
- `readIO()`: thay `delay(60)` cố định bằng `waitForBytes()` — chờ chủ động và thoát ngay khi RS485 có đủ byte phản hồi (vẫn giữ trần an toàn 60ms qua `RS485_READ_TIMEOUT_MS`), giảm đáng kể thời gian block mỗi lần đọc I/O trong điều kiện bình thường.
- `loop()`: thêm `IO_POLL_INTERVAL_MS` (120ms) để giãn nhịp đọc mirror I/O thay vì gọi `readIO()` ngay ở mọi vòng `loop()` rảnh — nhường chu kỳ CPU cho `acceptClients()`/`handleTCP()` xử lý lệnh relay từ ESP32 nhanh hơn, không bị kẹt phía sau vòng polling mirror.
- Thêm `FW_VERSION` và in ra Serial Monitor lúc khởi động để xác nhận board đang chạy đúng bản nào.

### [1.0.0] - 2026-06-18
Baseline ban đầu (chốt mốc trước khi cải tiến độ nhạy nút relay). Toàn bộ logic giữ nguyên như code gốc: TCP server 4 client, điều khiển relay qua RS485 (16 kênh), mirror input vật lý, scene switch, sync trạng thái sau lệnh.

---

## Code_ESP32_Web_UI_TCP_client_.ino

### [3.2.1] - 2026-06-29
Đổi thứ tự hiển thị tab theo yêu cầu vận hành: tab dùng nhiều nhất lên đầu.

- **Thứ tự mới**: 💡 Đèn M&A → 🔌 Đèn MEGA → 🧩 Đèn AMX → ❄️ Điều hòa LOGO! → 🖥️ KIOS → ⚙️ Cài đặt.
- Cập nhật `navIcons[]`/`navNames[]`, thứ tự page HTML trong SPA, `tvNames[]` trong form Hiển thị tab.
- Cập nhật `nav()` JS: KIOS load trigger chuyển sang `i===4`, pollCombo trigger chuyển sang `i===0`.
- Cập nhật `masterPoll()` JS: mapping `cur` → hàm poll đúng theo thứ tự mới.
- Bitmask `tab_vis` NVS đổi nghĩa: bit0=Đèn M&A, bit1=Đèn MEGA, bit2=Đèn AMX, bit3=Điều hòa, bit4=KIOS. Người dùng cần lưu lại "Hiển thị tab" sau khi nạp firmware nếu đã tùy chỉnh trước đó.

### [3.2.0] - 2026-06-27
Đổi tên tab + thêm trang ghép "Đèn MEGA & AMX".

- **Đổi tên tab**: "Đèn" → "Đèn MEGA", "Điều hòa" → "Điều hòa LOGO!", "AMX" → "Đèn AMX".
- **Trang mới "Đèn M&A"**: giao diện 16 nút giống hệt "Đèn MEGA", nhưng 4 vị trí điều khiển theo logic AMX (CE-REL8) thay vì MEGA:
  - Nút `3. P.Họp` → AMX Relay 1, `10. P.Tuấn` → Relay 2, `7. K.Doanh` → Relay 3, `2. H.Lang` → Relay 4.
  - 12 nút còn lại hoạt động như relay MEGA bình thường.
  - 4 nút đánh dấu `data-amx`; click gọi `/amx/relay`, 12 nút kia gọi `/cmd`.
  - "TẮT TẤT CẢ": tắt 16 relay MEGA (`/system/all off`) + tắt 4 relay AMX.
- **Poll tuần tự** trên trang ghép: `/status` (rẻ, cache) xong mới `/amx/status` → mỗi lúc chỉ 1 HTTP socket inbound, tránh va chạm 8 socket W5500. Khi rời trang: 0 tải thêm.
- Trang ghép có toggle ẩn/hiện trong "Hiển thị tab" (bit4). Bitmask `tab_vis` mở rộng 4→5 bit, mặc định `0x1F`. Bit0-3 giữ nguyên ý nghĩa cũ (không xáo trộn NVS).

### [3.1.0] - 2026-06-27
Fix "Lỗi kết nối" nhấp nháy trên điện thoại + giảm tải request lên W5500.

- **Nguyên nhân**: đồng hồ gọi `/info` mỗi 1s, cộng poll trang mỗi 2s → tại mốc 2s hai request bắn gần đồng thời, cần 2 socket W5500 cùng lúc (lại chồng `megaTransact` định kỳ 3s). Trên điện thoại (WiFi RTT cao, trình duyệt mobile giữ/mở socket khác desktop) thỉnh thoảng 1 request không giành được socket → `fetch` reject → `.catch()` hiện "Lỗi kết nối" ngay (zero dung sai). Desktop mạng nhanh hiếm khi va chạm nên không thấy.
- **Fix 1 — giảm tải**: đồng hồ tự chạy bằng JS (`tickClock` mỗi 1s), chỉ đồng bộ `/info` mỗi 30s (`syncInfo`) + khi vào tab Cài đặt. Bỏ ~28/30 request `/info`, xóa va chạm 1s/2s.
- **Fix 2 — dung sai lỗi**: chỉ hiện "Lỗi kết nối" sau 2 lần trượt liên tiếp (`_fM`/`_fA`/`_fC`), thành công reset ngay → ẩn các blip vô hại.

### [3.0.9] - 2026-06-27
Tinh chỉnh easter egg: ấn 3 lần (thay vì 5), giữ tiêu đề mục khi ẩn.

- Giảm số lần nhấn "Thông tin hệ thống" để mở khối cài đặt: **5 → 3 lần**.
- Khi ẩn: chỉ ẩn phần **chi tiết** (`.sdet`: các dòng nhập + nút lưu), **giữ lại tiêu đề** 3 mục (Hẹn giờ tắt thiết bị / Hiển thị tab / Đổi mật khẩu) để biết có những mục gì. Nhấn 3 lần để hiện/ẩn chi tiết.

### [3.0.8] - 2026-06-27
Đổi tên + ẩn cài đặt lịch, thêm tùy chọn ẩn/hiện tab.

- Đổi tiêu đề card "⏰ Tự động tắt thiết bị" → "⏰ Hẹn giờ tắt thiết bị".
- Gộp toàn bộ cài đặt nhạy cảm vào 1 khối ẩn `secret-body` — chỉ hiện khi nhấn tiêu đề "Thông tin hệ thống" 5 lần liên tiếp (easter egg, trước đây chỉ ẩn mục đổi mật khẩu): **Hẹn giờ tắt thiết bị**, **Hiển thị tab**, **Đổi mật khẩu**. Mặc định tab Cài đặt chỉ hiện card "Thông tin hệ thống".
- **Mới: Hiển thị tab** — 4 toggle bật/tắt hiển thị tab Đèn / Điều hòa / AMX / KIOS (tab Cài đặt luôn hiện để còn lối vào). Lưu vào NVS key `tab_vis` (bitmask, mặc định `0x0F` = hiện tất cả), tồn tại qua reboot.
- Nav bar render theo `tabVisible`: tab bị tắt → `display:none`. Lúc khởi động tự vào tab hiển thị đầu tiên (nếu Đèn bị ẩn).
- `GET /settings` thêm trường `tabs`; `POST /settings` thêm nhánh xử lý `tabs=<bitmask>`.

### [3.0.7] - 2026-06-27
Fix lỗi nghiêm trọng: sáng đến web ESP32 chết, ping vẫn OK, phải reset mới vào lại.

- **Nguyên nhân**: W5500 chỉ có 8 socket phần cứng. Khi client (browser/kiosk) đóng kết nối TRƯỚC, socket phía W5500 rơi vào trạng thái `CLOSE_WAIT`. `server.available()` chỉ trả về socket có data đến → socket `CLOSE_WAIT` (remote đã đóng, không còn data) không bao giờ được trả về → `client.stop()` không bao giờ được gọi lên chúng → kẹt vĩnh viễn. Qua đêm tích lũy đến khi cạn cả 8 socket: web server không accept được kết nối mới (web chết) nhưng ping vẫn OK vì ICMP do phần cứng W5500 xử lý, không cần socket.
- **Fix**: thêm `reapDeadSockets()` — quét cả 8 socket mỗi 2 giây, đóng những socket kẹt ở `CLOSE_WAIT` (status register `0x1C`) không còn data, giải phóng để tái sử dụng.
- `Connection: close` (đã có từ trước) chỉ xử lý đúng khi SERVER đóng trước; trường hợp CLIENT đóng trước cần reap chủ động.
- Không reset cả ESP32, không reset W5500 — chỉ đóng đúng socket bị rò.

### [3.0.6] - 2026-06-26
Thêm spinner "Đang tải..." overlay khi nhấn KC-Brain / SL-240C ở tab KIOS — phản hồi trực quan ngay, ẩn khi iframe load xong (`onload`).

### [3.0.5] - 2026-06-25
Form đổi mật khẩu ẩn sau easter egg 5-tap trên tiêu đề "Thông tin hệ thống".

- Form đổi mật khẩu không còn hiển thị mặc định — không có gợi ý UI từ bên ngoài.
- Nhấn nhanh vào tiêu đề **"ℹ️ Thông tin hệ thống"** 5 lần liên tiếp trong vòng 1.5s → form hiện/ẩn.
- JS: biến đếm `_itap` + `setTimeout` reset — không tốn tài nguyên.

### [3.0.4] - 2026-06-25
Form đổi mật khẩu thu gọn — nhấn tiêu đề để mở (thay thế bởi easter egg ở v3.0.5).

### [3.0.3] - 2026-06-25
Thêm ô "Nhập lại mật khẩu mới" vào form đổi mật khẩu — kiểm tra khớp phía JS trước khi gửi.

### [3.0.2] - 2026-06-25
Đổi mật khẩu qua Settings tab + fix bug lịch tự bật lại.

- **Đổi mật khẩu**: `authPass` chuyển từ `#define` sang biến `String` đọc từ NVS (key `auth_p`, mặc định `123456`). `POST /settings` xử lý param `old_p`/`new_p` — kiểm tra mật khẩu cũ, validate độ dài 4–32 ký tự, lưu vào NVS. Form gồm 3 ô: mật khẩu hiện tại, mật khẩu mới, nhập lại.
- **Fix**: `loadSched()` được gọi mỗi 2s bởi `masterPoll()` → ghi đè checkbox/input giờ trước khi người dùng kịp bấm Lưu. Fix: xóa nhánh `cur===4` khỏi `masterPoll()` — `loadSched()` chỉ chạy 1 lần khi vào tab qua `nav(4)`.

### [3.0.1] - 2026-06-25
Lưu settings vĩnh viễn vào flash NVS (Preferences).

- `#include <Preferences.h>`: thư viện NVS có sẵn trong ESP32 Arduino framework.
- `setup()`: đọc `sched_en`, `sched_h`, `sched_m` từ namespace `"tnn"` — áp dụng ngay khi boot. Lần đầu nạp firmware dùng default: bật, 18:00.
- `POST /settings`: sau khi cập nhật RAM, ghi đồng thời xuống NVS → tồn tại qua reboot và mất điện.
- Serial in `Settings loaded: sched=ON 18:00` khi boot để xác nhận.
- NVS chịu ~100.000 lần ghi (đổi giờ mỗi ngày = 273 năm).

### [3.0.0] - 2026-06-25
Thêm đồng hồ thực trên nav bar, tab Cài đặt, `/info` endpoint, nút Thoát.

- **Đồng hồ HH:MM:SS** trên nav bar — cập nhật mỗi giây qua `/info`, không tốn socket riêng.
- **Tab ⚙️ Cài đặt**:
  - Toggle bật/tắt scheduler.
  - Input giờ:phút tắt tự động — lưu qua `POST /settings`, có hiệu lực ngay không cần recompile.
  - Card thông tin hệ thống: firmware version, IP, NTP status, uptime.
- **`GET /info`**: JSON `{fw, uptime, time, ntp, sched}` — debug từ xa không cần Serial Monitor.
- **`GET /logout`**: xóa cookie `sid`, redirect về `/login`.
- **`GET /settings`** / **`POST /settings`**: đọc/ghi cấu hình scheduler.
- `schedEnabled` và `schedEntry` chuyển từ hằng số sang biến — thay đổi được qua UI.
- `masterPoll()`: thêm nhánh `cur===4` → `loadSched()`.
- `nav(4)`: tự động gọi `loadSched()` khi vào tab Cài đặt.

### [2.9.0] - 2026-06-25
Thay Basic Auth bằng form đăng nhập chỉ có ô Password (cookie-based session).

- Xóa hoàn toàn HTTP Basic Auth (không còn hộp thoại username/password của trình duyệt).
- Trang `/login`: form HTML tối giản 1 ô mật khẩu, nền tối, hiện "Sai mật khẩu" nếu sai.
- `POST /login`: kiểm tra password → set cookie `sid=<token>` → redirect `/`.
- `checkCookie()`: mọi request kiểm tra cookie trước khi xử lý.
- `initAuthToken()`: token ngẫu nhiên tạo từ `millis()` lúc boot — đổi mỗi lần khởi động.
- Fix NVS double-call: set `lastNtpAttempt` sau `ntpSync()` trong `setup()`.
- Mật khẩu mặc định: `123456` — đổi `AUTH_PASS` trước khi deploy.

### [2.8.0] - 2026-06-25
NTP time sync + scheduler tự động tắt thiết bị.

- `EthernetUDP` NTP client: sync giờ từ `216.239.35.0` (time.google.com) lúc boot, re-sync mỗi 6 tiếng, retry mỗi 30s nếu chưa sync.
- `runScheduler()`: kiểm tra mỗi giây, kích hoạt trong window 5 phút sau mốc giờ (xử lý trường hợp mất điện rồi boot lại).
- Mặc định: tắt tất cả lúc 18:00 — sửa `schedEntry` để đổi mốc.
- Tắt đồng thời: 16 relay MEGA + 4 relay AMX CE-REL8 + 4 điều hòa LOGO!.
- `schedFiredToday` (bitmask): mỗi mốc chỉ kích hoạt 1 lần/ngày, reset tự động khi sang ngày mới.

### [2.7.1] - 2026-06-25
Đổi mật khẩu mặc định thành `123456`. Đổi Basic Auth sang username rỗng (Base64 `":password"`) để trình duyệt chỉ hiện 1 ô Password. *(Thay thế hoàn toàn bởi v2.9.0)*

### [2.7.0] - 2026-06-25
Thêm HTTP Basic Authentication để bảo vệ giao diện khi mở cổng NAT ra ngoài Internet.

- Định nghĩa `AUTH_USER`, `AUTH_PASS`, `AUTH_B64` (Base64 của `user:pass`) trong config section đầu file — đổi tại đây trước khi nạp firmware.
- `checkAuth(request)`: đọc header `Authorization: Basic <token>`, so sánh với `AUTH_B64`.
- `send401(client)`: trả `401 Unauthorized` + header `WWW-Authenticate: Basic realm="TNN Smart Control"` để trình duyệt hiện hộp thoại đăng nhập.
- Kiểm tra ngay sau khi đọc xong HTTP header, trước khi xử lý bất kỳ route nào.
- Toàn bộ endpoint đều được bảo vệ: trang web, API relay, AC, AMX.
- Mặc định: user `tnn` / pass `tnn@2026` — **thay trước khi deploy ra Internet**.

### [2.6.0] - 2026-06-25
Refactor giao diện sang SPA (Single Page Application) — không load lại trang khi chuyển màn hình.

**Vấn đề**: Mỗi lần chuyển trang (Đèn → AMX → KIOS…) trình duyệt gửi HTTP GET mới, ESP32 phải build lại toàn bộ HTML (~800 dòng), socket W5500 mở/đóng liên tục. Trên kiosk không có nút Refresh, khi W5500 hết socket (TIME_WAIT tích lũy) toàn bộ giao diện mất luôn.

**Giải pháp**: Một route GET / duy nhất trả về toàn bộ HTML gồm 4 trang con (Đèn / Điều hòa / AMX / KIOS) ẩn bằng CSS (`display:none`). Nav bar 4 tab JS show/hide trang mà không cần HTTP request mới. `masterPoll()` chỉ fetch API của tab đang mở mỗi 2s — không poll mù cả 4 trang.

- Gộp 5 route HTML (/, /mega, /modbus, /amx, /kios) thành 1 route SPA duy nhất.
- Nav bar cố định đầu trang: **Đèn · Điều hòa · AMX · KIOS**.
- CSS `.page.active{display:flex}` — chuyển tab tức thì, không round-trip mạng.
- `masterPoll()`: `cur===0` → `pollMega()`, `cur===1` → `pollAC()`, `cur===2` → `pollAMX()` (KIOS không poll).
- KIOS tab: iframe nhúng với thanh địa chỉ, nút Tải lại và Mở tab mới.
- Fix lỗi biên dịch: xóa `kinp &&` thừa trước `client.println(...)`.
- Giữ nguyên toàn bộ API endpoint (/cmd, /status, /ac/toggle, /ac/status, /amx/relay, /amx/status).

### [2.5.0] - 2026-06-24
AMX CE-IO4: chuyển từ connect-per-transaction sang persistent connection, fix relay flicker khi boot.

- `amxIoConnect()`: kết nối bền vững tới CE-IO4, gửi `set /io/N/mode INPUT` + `set /io/N/inputMode DIGITAL` khi kết nối, seed `amxInputSnapshot` bằng cách đọc trạng thái thật trước khi bật toggle logic — tránh relay nháy khi ESP32 khởi động.
- `amxIoPoll()`: đọc response non-blocking mỗi vòng `loop()`.
- `amxIoSendGet()`: gửi 4 lệnh `get /io/N/digitalInput` trên persistent connection mỗi 600ms.
- Auto-reconnect CE-IO4 sau 5s nếu mất kết nối.
- Giải quyết ERR_CONNECTION_REFUSED: CE-IO4 mở socket mỗi 600ms tích lũy TIME_WAIT → socket W5500 cạn. Persistent connection loại bỏ hoàn toàn vòng lặp mở/đóng.

### [2.4.0] - 2026-06-24
AMX toggle logic: bấm công tắc tường → toggle relay (không map 1:1 trạng thái).

- `amxReconcile()`: sau 300ms, đọc relay thật → nếu relay chưa đổi (Kramer chưa xử lý) thì ESP32 toggle; nếu relay đã đổi (Kramer xử lý trước) thì bỏ qua.
- `amxPendingToggle`, `amxRelayBeforeIO`: tracking bit mask để biết kênh nào đang chờ reconcile.
- Tên nút relay AMX cập nhật: P.Họp / P.Tuấn / K.Doanh / H.Lang.

### [2.3.0] - 2026-06-24
AMX CE-IO4 polling: switch từ Subscribe → polling `get` mỗi 600ms.

- CE-IO4 firmware thực tế trả lỗi `"unknown path"` với lệnh `Subscribe` → chuyển sang polling `get /io/N/digitalInput` trên persistent connection.
- Fix IP CE-IO4: 192.168.1.7 (trước đó nhầm dùng 192.168.1.203 là CE-IRS4).
- `_parseAmxIoLine()`: parse `update /io/N/digitalInput true|false`, phát hiện thay đổi trạng thái, kích hoạt reconcile logic.

### [2.2.0] - 2026-06-24
AMX CE-IO4/CE-REL8 tích hợp ban đầu.

- Route `/amx/status`: poll CE-IO4 (4 digital input) + CE-REL8 (4 relay), trả JSON `{"relay":[...],"io":[...]}`.
- Route `/amx/relay?ch=N&value=true|false`: set relay CE-REL8 kênh N qua TCP AMX protocol.
- UI /amx: 4 nút relay đèn + 4 chỉ thị trạng thái công tắc tường IO1-IO4.
- Fix lỗi biên dịch `amxMirrorBlockUntil`: xóa tham chiếu stale sau refactor v2.0.0.

### [2.1.0] - 2026-06-24
Thêm trang KIOS và route /kios.

- Trang /kios: iframe nhúng KC-Brain, SL-240C, URL tùy chọn.
- Menu chính: thêm nút KIOS.

### [2.0.0] - 2026-06-24
Thay doi kien truc: bo megaClient persistent, dung connect-per-transaction cho MEGA (giong LOGO!). Giai quyet dut diem van de RECONNECTED OK loop. Chi tiet trong file.



### [1.9.0] - 2026-06-23
Cấu trúc lại giao diện web 4 trang: menu 4 nút (Đèn MEGA / Điều hòa LOGO! / AMX / Modbus TCP), /mega chỉ 16 relay (xóa AC section), /logo iframe LOGO!, /modbus placeholder, /amx giữ nguyên. Đơn giản hóa /cmd và /status.


### [1.8.4] - 2026-06-23
Sửa lỗi FC5 write không tới LOGO! dù Serial in "LOGO! FC5 ON" như thường.
- `logoTransact()`: thêm `c.flush()` ngay sau `c.write()` — bắt buộc với thư viện Ethernet/W5500 trên ESP32 để ép buffer TCP gửi ngay lập tức. Không có `flush()`, dữ liệu nằm trong TX buffer của W5500 và không được truyền cho đến khi kết nối đóng; khi đó LOGO! đã không còn socket để gửi lệnh.
- `logoFC5()`: đổi `expectedLen` từ `12` (chờ echo) → `0` (gửi xong đóng ngay, không chờ). Modbus write không cần xác nhận từ client, LOGO! xử lý lệnh ngay khi nhận frame. Tránh 200ms timeout không cần thiết mỗi lần bấm nút.
- `logoFC5()`: thêm log `sent` / `FAIL` để phân biệt kết nối thành công hay thất bại.

### [1.8.4] - 2026-06-23
Thêm giao diện webview nhúng trang điều khiển LOGO! Siemens (192.168.1.6) vào hệ thống:
- Menu chính (`/`): thêm nút "🌡 Điều khiển điều hòa (LOGO!)" trỏ tới `/logo`.
- Route `/logo` mới: trang fullscreen với thanh header (nút ⬅ Menu + tiêu đề + nút mở tab mới), bên dưới là `iframe` nhúng `http://192.168.1.6/webroot/main.htm`. Người dùng tương tác trực tiếp với giao diện LOGO! Web Editor mà không cần rời khỏi hệ thống ESP32.
- Fallback tự động: nếu LOGO! chặn iframe sau 3 giây (X-Frame-Options), trang ẩn iframe và hiển thị nút "Mở giao diện LOGO!" để mở tab mới thay thế.
- Nút "↗ Mở mới" luôn có sẵn trên header để mở LOGO! trong tab riêng (tiện hơn khi cần dùng song song).

### [1.8.3] - 2026-06-23
Sửa địa chỉ Modbus điều khiển sai. v1.8.x trước đó ghi vào M1-M4 (Coil 8257+) — đây là biến nội bộ của LOGO! Web UI, không phải đường API bên ngoài. Tài liệu hệ thống mô tả rõ 2 đường điều khiển riêng: M1-M4 cho LOGO! Web UI, và **V0.4-V0.7 (Coil 5-8)** cho Kramer/API bên ngoài. Kramer đang dùng đường này và đã được xác nhận hoạt động (Write Coil 5 ON → delay → OFF để kích AC1).
- `LOGO_M_ADDR`: đổi từ `8256` (M1 PDU 0-based) → `4` (V0.4 PDU 0-based, Coil 5 1-based).
- Địa chỉ feedback V0.0-V0.3 (`LOGO_FB_ADDR=0`) giữ nguyên — đúng theo tài liệu.

### [1.8.2] - 2026-06-23
Sửa lỗi LOGO! liên tục reconnect/disconnect (`LOGO! RECONNECTED OK` → ngắt ngay → lặp lại). Nguyên nhân: LOGO! Siemens không duy trì TCP connection idle — thiết bị công nghiệp tự đóng kết nối nếu không có traffic hợp lệ ngay sau khi kết nối.
- Bỏ hoàn toàn `EthernetClient logoClient` (persistent connection) và toàn bộ reconnect logic trong loop().
- Thêm `logoTransact(frame, len, resp, expectedLen)`: mỗi giao dịch Modbus tự **mở kết nối TCP mới → gửi frame → đọc response → đóng ngay**. Pattern này đáng tin cậy với thiết bị công nghiệp không giữ idle TCP.
- `logoFC5()` và `logoReadFeedback()` giờ dùng `logoTransact()` thay vì ghi/đọc qua persistent client.
- Overhead: ~10ms TCP handshake mỗi giao dịch trên LAN nội bộ — không đáng kể với chu kỳ poll 2.5s.
- Pulse state machine (non-blocking 500ms) vẫn giữ nguyên.
- Không cần connect LOGO! trong setup() hoặc loop().

### [1.8.1] - 2026-06-23
Sửa lỗi ESP32 reset liên tục do Watchdog Timer (`rst:0x8 TG1WDT_SYS_RESET`) ngay sau khi nạp v1.8.0.
- `setup()`: bỏ `logoClient.connect()` — kết nối TCP tới LOGO! trong `setup()` block CPU khi LOGO! chưa bật Modbus hoặc không phản hồi, đủ thời gian để ESP32 Watchdog nổ. Kết nối giờ chỉ được thử trong `loop()` mỗi 5s, nơi watchdog được feed đều đặn bởi Arduino task scheduler.
- `logoFC5()` và `logoReadFeedback()`: thêm `yield()` vào bên trong vòng busy-wait (thay vì vòng `while()` thuần), nhường CPU cho các task hệ thống ESP32 (watchdog feeder, WiFi stack, idle task) trong thời gian chờ phản hồi Modbus.

### [1.8.0] - 2026-06-23 (WATCHDOG BUG — xem v1.8.1)
Sửa lại toàn bộ logic Modbus TCP tới LOGO! sau khi đọc tài liệu kỹ thuật đầy đủ của hệ thống. v1.7.0 sai 2 điểm nghiêm trọng: (1) địa chỉ sai — ghi thẳng Q1-Q4 (8192+) thay vì M1-M4; (2) logic sai — set ON/OFF trực tiếp thay vì gửi xung toggle.

**Kiến trúc logic đúng trong LOGO!:**
Xung vào M1-M4 → B001-B004 (edge-triggered wiping relay, tạo xung 1s) → Q1-Q4 (relay) → giả lập nhấn nút vật lý trên bảng điều khiển ĐH. Feedback: I1-I4 → M21-M24 → NQ1-NQ4 → VM V0.0-V0.3.

**Modbus address map (LOGO! 8 0BA8):**
- Điều khiển: M1=Coil8257, M2=8258, M3=8259, M4=8260 (1-based) → PDU 8256-8259 (0-based, `LOGO_M_ADDR=8256`)
- Feedback: V0.0=Coil1, V0.1=2, V0.2=3, V0.3=4 → PDU 0-3 (`LOGO_FB_ADDR=0`)

**Thay đổi code:**
- `logoFC5(addr, val)`: helper gửi Modbus FC5 Write Single Coil và drain response.
- `logoSendPulse(ch)`: gửi xung toggle tới M bit — bật ON ngay, **NON-BLOCKING** (không dùng `delay(500)`); state machine trong `loop()` tắt OFF sau `LOGO_PULSE_MS=500ms`. Có per-channel cooldown 1.5s và chống pulse chồng nhau.
- `logoReadFeedback()`: FC1 đọc V0.0-V0.3 (PDU addr 0, qty 4) để lấy trạng thái thật điều hòa.
- `loop()`: thêm xử lý hoàn tất xung M bit (non-blocking check millis()), reconnect LOGO!, periodic poll feedback 2.5s.
- Route `/cmd`: `set /ac/x/pulse` → `logoSendPulse(ch)`. Format đổi từ `state true|false` sang `pulse` vì đây là toggle — LOGO! tự quyết trạng thái mới.
- JS AC buttons: gửi `set /ac/x/pulse`, optimistic toggle màu nút ngay (applyStatus() sẽ sửa lại đúng sau poll 2.5s từ feedback thật).
- CSS: thêm `button.on{background:green;}` sau `button.ac` để override màu xanh dương khi AC đang ON.

**Lưu ý khi test:**
- Nếu M bit không phản hồi: thử đổi `LOGO_M_ADDR` từ 8256 → 8257 (zero-based vs one-based ambiguity).
- Modbus Server phải được bật trong Logo! Soft Comfort.

### [1.7.0] - 2026-06-23 (SAI — xem v1.8.0)
Tích hợp điều khiển 4 điều hòa (AC1-AC4) qua Modbus TCP trực tiếp tới LOGO! Siemens (6ED1052-1FB08-0BA1) tại 192.168.1.6:502. Trước đây các nút AC gửi `set /ac/x/pulse` lên MEGA nhưng MEGA không xử lý (vô tác dụng).

Yêu cầu phần cứng/cấu hình: Modbus TCP Server phải được bật trong Logo! Soft Comfort (Device → Ethernet → Enable Modbus Server). LOGO! Q1=ĐH1 Kế Toán, Q2=ĐH2 P.Nguyên, Q3=ĐH3 P.Họp, Q4=ĐH4 P.Tuấn. Modbus coil base address Q1=8192 (0x2000, theo tài liệu LOGO! 8 0BA8) — điều chỉnh `LOGO_Q_BASE` nếu cần.
- Thêm `logoClient` (EthernetClient): kết nối Modbus TCP bền vững tới LOGO!, tự reconnect mỗi 5s nếu rớt (tương tự `megaClient`).
- Thêm `logoWriteCoil(ch, val)`: gửi Modbus FC5 (Write Single Coil) để bật/tắt Q1-Q4.
- Thêm `logoReadCoils()`: gửi Modbus FC1 (Read Coils), đọc trạng thái thật Q1-Q4 về `acSnapshot`.
- Periodic poll LOGO! mỗi 2.5s (`LOGO_POLL_MS`) để đồng bộ trạng thái AC lên UI.
- Route `/cmd`: lệnh `set /ac/x/state true|false` được xử lý qua Modbus TCP tới LOGO! (không chuyển sang MEGA), lệnh relay/system vẫn chuyển sang MEGA như cũ.
- Route `/status`: nối thêm `AC1=x,AC2=x,AC3=x,AC4=x` vào sau `L1-L16` — hàm `applyStatus()` phía JS tự nhận diện key AC1-AC4 và cập nhật màu nút (tận dụng cơ chế đã có, không cần thêm logic JS mới).
- CSS: thêm `button.on{background:green;}` để override `button.ac` (blue) khi AC button ở trạng thái ON — cần thiết vì CSS specificity của `.on` (10) thấp hơn `button.ac` (11).
- JS AC buttons: chuyển sang cơ chế `pendingTarget` + toggle state (giống relay buttons), thay cho `set /ac/x/pulse` vô tác dụng cũ.

### [1.6.0] - 2026-06-20
Rollback HTTP keep-alive (v1.5.0) — phát hiện lỗi response trả về status 200 OK nhưng body RỖNG (xác nhận qua DevTools tab Network), dù log Serial trên ESP32 khẳng định đã gửi đủ dữ liệu (`sendResponse: ... da gui xong`). Nghi vấn lỗi nằm ở tầng buffer/timing khi tái sử dụng socket trên thư viện Ethernet/W5500 — không lộ ra qua việc đọc lại logic code (logic đọc qua hợp lý), khó chẩn đoán/sửa tiếp một cách đáng tin cậy từ xa qua nhiều vòng debug. Quyết định: rủi ro/effort của việc tiếp tục debug keep-alive không tương xứng so với mức độ nghiêm trọng của vấn đề gốc (chỉ thỉnh thoảng rớt `megaClient` khi bấm RẤT nhanh) — trong khi giao diện hiện tại hoàn toàn không dùng được.
- Quay lại đúng cơ chế HTTP của v1.4.0: mỗi request mở 1 socket mới, `Connection: close`, `client.stop()` sau mỗi response — đã xác nhận chạy ổn định trong thực tế.
- Xóa `sendResponse()`, `acceptWebClients()`, `webClients[]`, `MAX_WEB_CLIENTS`, `WEB_IDLE_TIMEOUT_MS` (toàn bộ hạ tầng keep-alive).
- Giữ nguyên phần sửa lỗi JS `pendingTarget` (v1.4.0, bấm nhanh không bị triệt tiêu) — không liên quan tới keep-alive, vẫn đúng.
- Giải pháp thay thế cho vấn đề socket quá tải khi bấm dồn dập: tăng `CMD_COOLDOWN_MS` từ 90ms lên 150ms — giảm tần suất mở socket HTTP mới phía JS, giảm áp lực lên W5500 mà không cần giữ kết nối mở. Đây là phương án an toàn, rủi ro thấp đã đề xuất từ đầu trước khi thử keep-alive.

### [1.5.0] - 2026-06-20 (ROLLBACK — xem v1.6.0)
~~Triển khai HTTP keep-alive: giữ tối đa 2 kết nối HTTP mở lâu dài, gửi Content-Length chính xác, không đóng kết nối sau mỗi response.~~ Đã rollback ở v1.6.0 do lỗi response body rỗng không tìm ra nguyên nhân gốc sau nhiều vòng debug. Giữ lại đoạn này để truy vết lịch sử thay đổi.

### [1.4.0] - 2026-06-20
Sửa lỗi: bấm nhanh liên tiếp 2 lần vào 1 nút relay không có tác dụng ("phải giữ tay/chuột đè 1 chút mới ăn"), xảy ra cả với chuột và cảm ứng. Nguyên nhân: `onclick` của mỗi nút đọc trực tiếp `classList.contains('on')` để tính trạng thái mới mỗi lần bấm — nếu bấm 2 lần rất nhanh, lần bấm thứ 2 đọc đúng màu đã bị lần 1 vừa đổi và đảo ngược lại lần nữa, khiến 2 lần bấm tự triệt tiêu nhau cả về màu hiển thị lẫn lệnh cuối cùng được gửi đi. Một lần bấm/giữ đủ lâu chỉ tạo đúng 1 sự kiện click nên không gặp lỗi này.
- Thêm `pendingTarget{}`: theo dõi trạng thái đích mong muốn riêng cho từng nút relay (độc lập với `classList`), mỗi lần bấm đảo đúng 1 nấc so với lần bấm ngay trước đó, không phụ thuộc việc DOM đã kịp cập nhật hay chưa.
- `applyStatus()`: đồng bộ lại `pendingTarget` theo trạng thái thật mỗi khi nhận dữ liệu từ `/status`, tránh lệch vĩnh viễn nếu có lệnh bị rớt.
- Nút "TẮT TẤT CẢ": xóa `pendingTarget` về rỗng khi bấm, để lần bấm tiếp theo trên từng nút tính lại đúng từ trạng thái mới.
Rollback phần block đồng bộ của v1.2.0 — sau khi test thực tế, v1.2.0 khiến độ nhạy quay lại y như bản gốc (5-8 lần bấm), TỆ HƠN so với v1.1.0 (3-5 lần). Nguyên nhân: route `/cmd` ở v1.2.0 chờ đồng bộ (block tối đa 150ms) để lấy trạng thái thật từ MEGA trước khi trả lời — nhưng MEGA có vòng polling I/O định kỳ (~120ms, phục vụ mirror công tắc vật lý) khiến nó không đọc kịp lệnh TCP mới trong lúc đang polling. Vì chu kỳ polling đó gần trùng với nhịp bấm nút dồn dập, gần như mọi lần bấm đều rơi đúng lúc MEGA bận, khiến ESP32 phải chờ sát ngưỡng 150ms — lộ thẳng độ trễ xử lý của MEGA ra cho trình duyệt, thay vì giấu nó đi như trước.
- Xóa `waitMegaStatus()` và bỏ việc chờ đồng bộ trong route `/cmd` — trả lời ngay lập tức ("OK") như trước v1.2.0.
- JS (`/mega`): khôi phục cơ chế cũ — sau khi gửi `/cmd`, gọi `poll()` không-block (`setTimeout(poll, 80)`) để xác nhận trạng thái thật sớm mà không chặn vòng lặp chính của ESP32. `setInterval(poll, 2000)` vẫn giữ nguyên cho đồng bộ định kỳ.
- Đi kèm với bản này là MEGA v1.2.0 (xem mục tương ứng) — phần sửa thực sự giải quyết gốc rễ độ trễ nằm ở phía MEGA: ưu tiên xử lý lệnh TCP hơn vòng polling mirror I/O.

### [1.2.0] - 2026-06-18 (ROLLBACK — xem v1.3.0)
~~Giảm thêm độ trễ mỗi lần bấm nút relay bằng cách gộp `/cmd` + `/status` thành 1 round-trip có chờ đồng bộ.~~ Đã rollback do gây regression (xem giải thích ở v1.3.0). Giữ lại đoạn này để truy vết lịch sử thay đổi.

### [1.1.0] - 2026-06-18
Tránh mất/lỗi request khi gói TCP bị chia nhỏ:
- `handleWebRequest()`: thêm vòng chờ có timeout (`REQUEST_TIMEOUT_MS` = 500ms) để đọc đủ header HTTP (`\r\n\r\n`) trước khi parse, thay vì bỏ cuộc ngay khi `client.available()` tạm thời về 0 — trước đây có thể khiến request bị đọc cụt, không khớp route nào, lệnh bị âm thầm bỏ qua mà không có phản hồi lỗi rõ ràng.
- Thêm `FW_VERSION` và in ra Serial Monitor lúc khởi động.

### [1.0.0] - 2026-06-18
Baseline ban đầu (chốt mốc trước khi cải tiến độ nhạy nút relay). Web UI menu/mega/amx, API `/set`, `/cmd`, `/status`, `/api/status`, TCP client tới MEGA với auto-reconnect mỗi 3s.
