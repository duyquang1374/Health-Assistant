#include <WiFi.h>
#include <ESP_Mail_Client.h>
#include <ArduinoJson.h>
#include "WebSocketMCP.h"
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"   // đi kèm thư viện SparkFun MAX3010x
#include "secrets.h"
#include "health_types.h"

// ====== OLED ======
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ====== DHT11 ======
#define DHTPIN 23
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ====== MAX30102 ======
MAX30105 particleSensor;
bool sensorOk = false;
#define FINGER_THRESHOLD   50000   // IR lớn hơn mức này là có ngón tay
#define FINGER_WAIT_MS      5000   // chờ ngón tay tối đa 5 giây
#define BUFFER_LEN           100   // 100 mẫu ở 25 sps = 4 giây
#define MEASURE_ROUNDS         4   // 4 lần tính: tổng khoảng 7 giây

// ====== LED 3 MỨC ======
#define LED_GREEN  19
#define LED_YELLOW 18
#define LED_RED     5

// ====== NÚT KHẨN CẤP (giữ 2 giây mới gửi) ======
#define EMERGENCY_BUTTON_PIN 4
#define BUTTON_HOLD_MS       2000
#define EMAIL_COOLDOWN_MS    60000

// ====== TÍN HIỆU BÁO SANG BOARD CHATBOT (nối dây + chung GND) ======
#define SIGNAL_BUSY_PIN 25   // HIGH trong lúc đang gửi email
#define SIGNAL_OK_PIN   26   // HIGH 5 giây nếu gửi thành công

// ====== I2C ======
#define I2C_SDA 21
#define I2C_SCL 22

// ====== MCP / SMTP ======
const char* mcpEndpoint = MCP_ENDPOINT;
WebSocketMCP mcpClient;

#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465
SMTPSession smtp;
void smtpCallback(SMTP_Status status) { Serial.println(status.info()); }

// ====== BIẾN TOÀN CỤC ======
float temperature = 0;
float humidity = 0;
bool tempValid = false;
int lastHR = 0;
int lastSpO2 = 0;
int lastLevel = 0;               // 0 chưa đo, 1 xanh, 2 vàng, 3 đỏ
unsigned long lastSensorRead = 0;
unsigned long ledTurnOnTime = 0;
unsigned long lastEmailTime = 0;
unsigned long okSignalTime = 0;

static uint32_t irBuf[BUFFER_LEN];
static uint32_t redBuf[BUFFER_LEN];

// ---------- LED ----------
void setLed(int level) {
    digitalWrite(LED_GREEN,  level == 1);
    digitalWrite(LED_YELLOW, level == 2);
    digitalWrite(LED_RED,    level == 3);
}

// Trả LED về đúng trạng thái của lần đo gần nhất (hoặc tắt hết nếu không còn lần đo nào đang hiển thị).
// Dùng sau khi gửi email khẩn cấp để đèn đỏ khẩn cấp không bị kẹt sáng.
void restoreLed() {
    if (ledTurnOnTime > 0) setLed(lastLevel);
    else setLed(0);
}

// Ngưỡng tham khảo, nên nhờ bác sĩ xem lại trước khi dùng thật
int classify(int hr, int spo2) {
    if (spo2 < 90 || hr < 50 || hr > 120) return 3;
    if (spo2 < 95 || hr < 60 || hr > 100) return 2;
    return 1;
}

// ---------- OLED ----------
void showMessage(const char* l1, const char* l2 = "") {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 16);
    display.println(l1);
    display.setCursor(0, 32);
    display.println(l2);
    display.display();
}

void updateOLED() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Nhiet do MT: ");
    if (tempValid) display.print(temperature, 1); else display.print("--");
    display.print(" C");

    display.setTextSize(2);
    display.setCursor(0, 14);
    display.print("HR ");
    if (lastHR > 0) display.print(lastHR); else display.print("--");

    display.setCursor(0, 32);
    display.print("SpO2 ");
    if (lastSpO2 > 0) { display.print(lastSpO2); display.print("%"); } else display.print("--");

    display.setTextSize(1);
    display.setCursor(0, 54);
    if (lastLevel == 1)      display.print("Binh thuong");
    else if (lastLevel == 2) display.print("Can chu y");
    else if (lastLevel == 3) display.print("NGUY HIEM! GIU NUT VANG");
    else                     display.print("Cho do nhip tim...");

    display.display();
}

// ---------- Email ----------
bool sendEmergencyEmail(const char* reason) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[EMAIL] WiFi chua ket noi!");
        return false;
    }
    if (lastEmailTime != 0 && millis() - lastEmailTime < EMAIL_COOLDOWN_MS) {
        Serial.println("[EMAIL] Vua gui xong, bo qua de tranh gui lien tuc.");
        return true;
    }

    ESP_Mail_Session session;
    session.server.host_name = SMTP_HOST;
    session.server.port = SMTP_PORT;
    session.login.email = AUTHOR_EMAIL;
    session.login.password = AUTHOR_PASSWORD;
    session.login.user_domain = "";
    session.time.ntp_server = F("pool.ntp.org,time.nist.gov");
    session.time.gmt_offset = 7;
    session.time.day_light_offset = 0;

    SMTP_Message message;
    message.sender.name = "Tro ly suc khoe";
    message.sender.email = AUTHOR_EMAIL;
    message.subject = "KHAN CAP: Ong Ba can giup do ngay!";
    message.addRecipient("Nguoi than", RECIPIENT_EMAIL);

    String text = String(reason) + "\n\n";
    if (lastHR > 0) {
        text += "Chi so do gan nhat:\n";
        text += "- Nhip tim: " + String(lastHR) + " bpm\n";
        text += "- SpO2: " + String(lastSpO2) + " %\n\n";
    } else {
        text += "Chua co lan do nao gan day.\n\n";
    }
    text += "Vui long goi dien ve nha hoac quay ve ho tro ngay.";
    message.text.content = text.c_str();

    if (!smtp.connect(&session)) {
        Serial.println("[EMAIL] Khong the ket noi Gmail SMTP.");
        return false;
    }
    if (!MailClient.sendMail(&smtp, &message)) {
        Serial.println("[EMAIL] Loi gui mail: " + smtp.errorReason());
        return false;
    }
    Serial.println("[EMAIL] Da gui thanh cong.");
    lastEmailTime = millis();   // chỉ khóa 60 giây khi đã gửi THÀNH CÔNG
    return true;
}

// ---------- Đo nhịp tim và SpO2 ----------
static bool readSample(uint32_t &red, uint32_t &ir, unsigned long timeoutMs) {
    unsigned long t = millis();
    while (!particleSensor.available()) {
        particleSensor.check();
        if (millis() - t > timeoutMs) return false;
        delay(1);
    }
    red = particleSensor.getFIFORed();
    ir  = particleSensor.getFIFOIR();
    particleSensor.nextSample();
    return true;
}

Vitals measureVitals() {
    Vitals v = {M_BAD_SIGNAL, 0, 0};
    if (!sensorOk) { v.status = M_SENSOR_ERROR; return v; }

    particleSensor.wakeUp();
    particleSensor.clearFIFO();

    // 1. Chờ ngón tay
    bool finger = false;
    unsigned long t0 = millis();
    while (millis() - t0 < FINGER_WAIT_MS) {
        if (particleSensor.getIR() > FINGER_THRESHOLD) { finger = true; break; }
        delay(50);
    }
    if (!finger) {
        particleSensor.shutDown();
        v.status = M_NO_FINGER;
        return v;
    }
    showMessage("Dang do...", "Giu yen ngon tay");
    delay(800);                       // chờ tín hiệu ổn định
    particleSensor.clearFIFO();

    // 2. Lấp đầy bộ đệm 100 mẫu
    for (int i = 0; i < BUFFER_LEN; i++) {
        if (!readSample(redBuf[i], irBuf[i], 1000)) {
            particleSensor.shutDown();
            v.status = M_SENSOR_ERROR;
            return v;
        }
    }

    // 3. Tính, rồi trượt 25 mẫu mới và tính lại, lấy trung bình các lần hợp lệ
    int32_t spo2, hr;
    int8_t validSpo2, validHr;
    int validCount = 0;
    long sumHr = 0, sumSpo2 = 0;

    for (int r = 0; r < MEASURE_ROUNDS; r++) {
        if (r > 0) {
            memmove(redBuf, redBuf + 25, (BUFFER_LEN - 25) * sizeof(uint32_t));
            memmove(irBuf,  irBuf + 25,  (BUFFER_LEN - 25) * sizeof(uint32_t));
            for (int i = BUFFER_LEN - 25; i < BUFFER_LEN; i++) {
                if (!readSample(redBuf[i], irBuf[i], 1000)) {
                    particleSensor.shutDown();
                    v.status = M_SENSOR_ERROR;
                    return v;
                }
            }
        }
        if (irBuf[BUFFER_LEN - 1] < FINGER_THRESHOLD / 2) {   // rút tay giữa chừng
            particleSensor.shutDown();
            v.status = M_NO_FINGER;
            return v;
        }
        maxim_heart_rate_and_oxygen_saturation(irBuf, BUFFER_LEN, redBuf,
                                               &spo2, &validSpo2, &hr, &validHr);
        Serial.printf("[DO] vong %d: HR=%ld(%d) SpO2=%ld(%d)\n", r, (long)hr, validHr, (long)spo2, validSpo2);
        if (validSpo2 && validHr && spo2 >= 70 && spo2 <= 100 && hr >= 30 && hr <= 220) {
            sumHr += hr; sumSpo2 += spo2; validCount++;
        }
    }
    particleSensor.shutDown();

    if (validCount < 2) {              // không đủ dữ liệu tin cậy thì báo lỗi, KHÔNG bịa số
        v.status = M_BAD_SIGNAL;
        return v;
    }
    v.status = M_OK;
    v.hr = sumHr / validCount;
    v.spo2 = sumSpo2 / validCount;
    return v;
}

// ---------- MCP ----------
static bool argHandsWarmed(const String& args) {
    DynamicJsonDocument d(256);
    if (deserializeJson(d, args)) return false;
    return d["hands_warmed"] | false;
}

void registerMcpTools() {
    mcpClient.registerTool(
        "check_environment",
        "Kiem tra nhiet do phong. Goi tool nay truoc khi do nhip tim neu can biet co can lam am tay khong.",
        R"({"type":"object","properties":{},"required":[]})",
        [](const String& args) {
            DynamicJsonDocument doc(256);
            if (!tempValid) {
                doc["status"] = "sensor_error";
                doc["message"] = "Khong doc duoc nhiet do. Hay noi that voi nguoi dung.";
            } else {
                doc["status"] = "ok";
                doc["temperature_c"] = temperature;
                doc["needs_warm_hands"] = temperature < 20.0;
                doc["message"] = "Hay doc nhiet do cho nguoi dung. Neu needs_warm_hands la true, nhac xoa hai ban tay cho am truoc khi do.";
            }
            String json;
            serializeJson(doc, json);
            return WebSocketMCP::ToolResponse(json);
        }
    );

    mcpClient.registerTool(
        "measure_health",
        "Do nhip tim va SpO2 bang cam bien dau ngon tay, mat khoang 15 giay. Chi goi khi nguoi dung yeu cau do. "
        "Truoc khi goi, bao nguoi dung dat nhe ngon tay len cam bien va giu yen. "
        "Neu ket qua la cold_warning, nhac lam am tay roi chi goi lai voi hands_warmed=true sau khi nguoi dung xac nhan. "
        "Sau khi co ket qua, doc so do va dua ra loi khuyen theo level. Neu khuyen nguoi dung an nut khan cap (nut vang), luon nhac ro an giu trong khoang 2 giay roi moi nha ra. Khong chan doan benh.",
        R"({"type":"object","properties":{"hands_warmed":{"type":"boolean","description":"true neu nguoi dung da xac nhan tay da am"}},"required":[]})",
        [](const String& args) {
            Serial.println("\n=== Xiaozhi yeu cau do suc khoe ===");
            bool warmed = argHandsWarmed(args);
            DynamicJsonDocument doc(768);

            if (tempValid && temperature < 20.0 && !warmed) {
                doc["status"] = "cold_warning";
                doc["temperature_c"] = temperature;
                doc["message"] = "Nhiet do phong duoi 20 do. Hay nhac nguoi dung xoa hai ban tay cho am, doi ho xac nhan, roi goi lai measure_health voi hands_warmed=true.";
                String json; serializeJson(doc, json);
                return WebSocketMCP::ToolResponse(json);
            }

            setLed(0);
            Vitals v = measureVitals();

            switch (v.status) {
            case M_NO_FINGER:
                doc["status"] = "no_finger";
                doc["message"] = "Khong phat hien ngon tay. Nhac nguoi dung dat vua khit dau ngon tay len cam bien, giu yen, roi hoi co muon do lai khong.";
                updateOLED();
                break;
            case M_BAD_SIGNAL:
                doc["status"] = "bad_signal";
                doc["message"] = "Tin hieu chua ro. Nhac giu yen tay, tay am, khong an qua manh, roi de nghi do lai.";
                updateOLED();
                break;
            case M_SENSOR_ERROR:
                doc["status"] = "sensor_error";
                doc["message"] = "Cam bien dang gap truc trac. Noi that voi nguoi dung va nho nguoi nha kiem tra.";
                updateOLED();
                break;
            default: {
                lastHR = v.hr;
                lastSpO2 = v.spo2;
                lastLevel = classify(v.hr, v.spo2);
                setLed(lastLevel);
                ledTurnOnTime = millis();
                updateOLED();

                doc["status"] = "success";
                doc["heart_rate"] = lastHR;
                doc["spo2"] = lastSpO2;
                doc["level"] = lastLevel == 1 ? "green" : (lastLevel == 2 ? "yellow" : "red");
                if (lastLevel == 3) {
                    doc["message"] = "Chi so NGUY HIEM. Doc so do, bao nguoi dung ngoi yen, hoi co kho tho, tuc nguc, chong mat khong. Neu co, goi send_emergency_email va khuyen goi 115. Noi ro: 'Ong ba hay AN GIU NUT VANG tren hop de chau bao nguoi nha ngay a!'. May do chi de tham khao.";
                } else if (lastLevel == 2) {
                    doc["message"] = "Chi so CAN CHU Y. Doc so do, khuyen ngoi nghi 5 den 10 phut, tho sau cham roi do lai. Neu van bat thuong hoac thay met, nen bao nguoi nha hoac di kham. May do chi de tham khao.";
                } else {
                    doc["message"] = "Chi so BINH THUONG. Doc so do va khen nhe, nhac uong du nuoc va giu thoi quen tot. Neu nguoi dung phan nan chong mat, buon non, dau dau hoac met, khuyen an NUT GIU VANG de bao nguoi nha. May do chi de tham khao.";
                }
            }
            }
            String json;
            serializeJson(doc, json);
            Serial.println("[DO] Hoan tat.");
            return WebSocketMCP::ToolResponse(json);
        }
    );

    mcpClient.registerTool(
        "send_emergency_email",
        "Gui email canh bao khan cap cho nguoi than. Chi goi khi nguoi dung nho bao nguoi nha, hoac chi so o muc do kem trieu chung nguy hiem.",
        R"({"type":"object","properties":{},"required":[]})",
        [](const String& args) {
            setLed(3);
            bool ok = sendEmergencyEmail("Tro ly suc khoe gui canh bao theo yeu cau hoac do chi so bat thuong.");
            restoreLed();                       // gửi xong thì trả đèn về trạng thái trước đó
            DynamicJsonDocument doc(256);
            doc["status"] = ok ? "sent" : "failed";
            doc["message"] = ok ? "Da gui email cho nguoi than. Tran an nguoi dung."
                                : "Gui email that bai. Khuyen nguoi dung goi 115 hoac nguoi nha truc tiep.";
            String json; serializeJson(doc, json);
            return WebSocketMCP::ToolResponse(json);
        }
    );

    Serial.println("[MCP] Registered: check_environment, measure_health, send_emergency_email");
}

void onConnectionStatus(bool connected) {
    if (connected) {
        Serial.println("[MCP] Da ket noi");
        registerMcpTools();
    } else {
        Serial.println("[MCP] Mat ket noi MCP");
    }
}

// ---------- setup / loop ----------
void setup() {
    Serial.begin(115200);

    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_YELLOW, OUTPUT);
    pinMode(LED_RED, OUTPUT);
    pinMode(EMERGENCY_BUTTON_PIN, INPUT_PULLUP);
    pinMode(SIGNAL_BUSY_PIN, OUTPUT);
    pinMode(SIGNAL_OK_PIN, OUTPUT);
    digitalWrite(SIGNAL_BUSY_PIN, LOW);
    digitalWrite(SIGNAL_OK_PIN, LOW);
    setLed(0);

    smtp.callback(smtpCallback);
    Wire.begin(I2C_SDA, I2C_SCL);
    dht.begin();

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED ERROR!");
    }
    display.clearDisplay();
    display.display();

    if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
        Serial.println("MAX30102 ERROR! Kiem tra day noi.");
    } else {
        // sáng LED 60, trung bình 4, chế độ Red+IR, 100 sps (còn 25 sps), xung 411us, ADC 4096
        particleSensor.setup(60, 4, 2, 100, 411, 4096);
        particleSensor.shutDown();    // chỉ bật khi đo
        sensorOk = true;
    }

    showMessage("Dang ket noi WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(300);
    }
    Serial.println("\nWiFi connected!");

    mcpClient.begin(mcpEndpoint, onConnectionStatus);
    updateOLED();
}

void loop() {
    mcpClient.loop();
    unsigned long now = millis();

    // DHT11 mỗi 2 giây
    if (now - lastSensorRead >= 2000) {
        lastSensorRead = now;
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t) && !isnan(h)) {
            temperature = t;
            humidity = h;
            tempValid = true;
            updateOLED();
        }
    }

    // Tự tắt LED sau 30 giây (trừ mức đỏ của lần đo để người nhà còn thấy)
    if (ledTurnOnTime > 0 && lastLevel != 3 && now - ledTurnOnTime > 30000) {
        setLed(0);
        ledTurnOnTime = 0;
    }

    // Hạ tín hiệu OK sau 5 giây
    if (okSignalTime > 0 && now - okSignalTime > 5000) {
        digitalWrite(SIGNAL_OK_PIN, LOW);
        okSignalTime = 0;
    }

    // Nút khẩn cấp: giữ 2 giây mới gửi
    static unsigned long pressStart = 0;
    static bool fired = false;
    if (digitalRead(EMERGENCY_BUTTON_PIN) == LOW) {
        if (pressStart == 0) pressStart = now;
        if (!fired && now - pressStart >= BUTTON_HOLD_MS) {
            fired = true;
            Serial.println("NUT KHAN CAP DUOC GIU!");
            setLed(3);
            showMessage("DANG GUI EMAIL...", "Cho nguoi than");
            digitalWrite(SIGNAL_BUSY_PIN, HIGH);          // chatbot nói "đã nhận được yêu cầu"
            delay(300);                                    // giữ đủ lâu để chatbot nhận ra tín hiệu
            bool ok = sendEmergencyEmail("Ong Ba vua giu NUT VANG khan cap.");
            if (ok) { digitalWrite(SIGNAL_OK_PIN, HIGH); okSignalTime = millis(); }
            digitalWrite(SIGNAL_BUSY_PIN, LOW);           // hạ sau OK để chatbot đọc đúng kết quả
            restoreLed();                                  // gửi xong: tắt đèn đỏ khẩn cấp
            showMessage(ok ? "DA GUI!" : "GUI LOI!", ok ? "Da bao nguoi than" : "Hay goi 115");
            delay(2000);
            updateOLED();
        }
    } else {
        pressStart = 0;
        fired = false;
    }
}