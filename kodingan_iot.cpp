// --- Konfigurasi Blynk ---
#define BLYNK_TEMPLATE_ID   "TMPL6R4TkcaUh"
#define BLYNK_TEMPLATE_NAME "Jemuran Otomatis"
#define BLYNK_AUTH_TOKEN    "Zy3NyFoA-BBz2bDUumIN6YqCpjdB7P_l"

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <AccelStepper.h>
#include <Preferences.h>
#include <EEPROM.h>

// --- Data WiFi ---
char ssid[] = "ltsc";
char pass[] = "tolol123";

// --- Pin Perangkat ---
#define PIN_MOTOR_IN1 13
#define PIN_MOTOR_IN2 12
#define PIN_MOTOR_IN3 14
#define PIN_MOTOR_IN4 27
#define PIN_HUJAN 34
#define PIN_CAHAYA 35

// --- Kalibrasi Posisi dan Sensor ---
const int POSISI_DALAM = 0;      
const int POSISI_LUAR  = 4000;   
const int BATAS_HUJAN  = 2800; 
const int BATAS_GELAP  = 3400; 

// --- Ukuran Stack untuk Task ---
#define STACK_MOTOR 4096 
#define STACK_IOT   8192 

// --- Objek Utama ---
AccelStepper motor(AccelStepper::HALF4WIRE, PIN_MOTOR_IN1, PIN_MOTOR_IN3, PIN_MOTOR_IN2, PIN_MOTOR_IN4);
Preferences memori; 

// --- Status Sistem ---
volatile bool sistemAktif = true;       
volatile bool modeOtomatis = true; 
volatile bool modeManual = false;  

// --- Cache Dashboard ---
String cachedCuaca = "";
String cachedCahaya = "";
String cachedPosisi = "";
long posisiTersimpan = -1; 

// --- Debouncing Button ---
unsigned long waktuTekanTerakhir = 0;
const unsigned long DEBOUNCE_DELAY = 300; // 300ms

// --- Alamat EEPROM untuk Auto-Resume ---
#define ALAMAT_FLAG_RESUME   0x10
#define ALAMAT_TARGET_RESUME 0x12
#define ALAMAT_POSISI_RESUME 0x14

// --- Deklarasi Fungsi ---
void TaskMotor(void *pvParameters);
void TaskIoT(void *pvParameters);
void jalankanLogika();
void perbaharuiDashboard(String cuaca, String cahaya);
void simpanPosisi(bool paksa);
void simpanTarget(int target);
void simpanMode();
void simpanDataResume(int target, long posisi);
void hapusDataResume();
bool cekPerluResume();
void ambilDataResume(int &target, long &posisi);
void jalankanAutoResumeJikaPerlu();

// --- Setup Awal ---
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] Memulai sistem jemuran otomatis...");

  EEPROM.begin(64); // Inisialisasi EEPROM
  memori.begin("jemuran", false); 

  // --- Set Parameter Motor ---
  motor.setMaxSpeed(1000);
  motor.setAcceleration(1000);

  // --- Ambil Data dari Memori ---
  int posisiSebelumnya = memori.getInt("posisi", 0); 
  int targetSebelumnya = memori.getInt("target", 0);
  modeOtomatis = memori.getBool("modeAuto", true); 
  modeManual = memori.getBool("modeManual", false);

  Serial.printf("[MEM] Posisi: %d | Target: %d\n", posisiSebelumnya, targetSebelumnya);

  // --- Cek Auto-Resume Saat Boot ---
  jalankanAutoResumeJikaPerlu();
  if (!cekPerluResume()) {
    motor.setCurrentPosition(posisiSebelumnya); 
    motor.moveTo(targetSebelumnya); 
  }

  // --- Buat Task untuk Dual Core ---
  xTaskCreatePinnedToCore(TaskMotor, "MotorCtrl", STACK_MOTOR, NULL, 2, NULL, 1); // Core 1
  xTaskCreatePinnedToCore(TaskIoT, "IoTLogic", STACK_IOT, NULL, 1, NULL, 0);     // Core 0
  
  Serial.println("[BOOT] Sistem siap beroperasi!");
}

void loop() { 
  vTaskDelete(NULL); 
}

// --- Task Motor (Core 1) ---
void TaskMotor(void *pvParameters) {
  unsigned long waktuSimpanTerakhir = 0;
  for (;;) {
    motor.run(); 
    // Simpan posisi setiap 1 detik saat bergerak
    if (motor.isRunning()) {
      if (millis() - waktuSimpanTerakhir > 1000) { 
        simpanPosisi(false); 
        waktuSimpanTerakhir = millis();
      }
    } else {
      if (motor.distanceToGo() == 0) {
        simpanPosisi(false);
      }
    }
    vTaskDelay(1 / portTICK_PERIOD_MS); 
  }
}

// --- Task IoT (Core 0) ---
void TaskIoT(void *pvParameters) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Blynk.config(BLYNK_AUTH_TOKEN); 
  unsigned long waktuCekWifi = 0;
  int jumlahReconnect = 0;
  for (;;) {
    // Koneksi WiFi & Blynk
    if (WiFi.status() == WL_CONNECTED) {
      if (!Blynk.connected()) {
        Blynk.connect();
      } else {
        Blynk.run();
      }
      jumlahReconnect = 0;
    } else {
      if (millis() - waktuCekWifi > 10000) { 
        waktuCekWifi = millis();
        jumlahReconnect++;
        Serial.printf("[WIFI] Reconnect attempt %d...\n", jumlahReconnect);
        WiFi.reconnect();
        if (jumlahReconnect >= 5) {
          Serial.println("[WIFI] Restart WiFi module...");
          WiFi.disconnect();
          delay(1000);
          WiFi.begin(ssid, pass);
          jumlahReconnect = 0;
        }
      }
    }
    jalankanLogika();
    vTaskDelay(200 / portTICK_PERIOD_MS); 
  }
}

// --- Simpan Posisi Motor ke Flash ---
void simpanPosisi(bool paksa) {
  long posisiSekarang = motor.currentPosition();
  if (paksa || posisiSekarang != posisiTersimpan) {
    memori.putInt("posisi", posisiSekarang); 
    posisiTersimpan = posisiSekarang;
  }
}

// --- Simpan Target Perjalanan ---
void simpanTarget(int target) {
  memori.putInt("target", target);
  Serial.printf("[MEM] Target: %d | Posisi: %d\n", target, motor.currentPosition());
}

// --- Simpan Mode Operasi ---
void simpanMode() {
  memori.putBool("modeAuto", modeOtomatis);
  memori.putBool("modeManual", modeManual);
  Serial.printf("[MODE] Auto: %d | Manual: %d\n", modeOtomatis, modeManual);
}

// --- Logika Utama Sistem ---
void jalankanLogika() {
  int nilaiHujan = analogRead(PIN_HUJAN);
  int nilaiCahaya = analogRead(PIN_CAHAYA);

  if (!sistemAktif) {
    perbaharuiDashboard("OFF", "-");
    if (Blynk.connected()) {
      Blynk.virtualWrite(V7, "SISTEM MATI");
    }
    if (motor.distanceToGo() != 0) {
      motor.moveTo(motor.currentPosition());
      simpanTarget(motor.currentPosition());
    }
    return;
  }

  String statusCuaca = (nilaiHujan < BATAS_HUJAN) ? "HUJAN" : "KERING";
  String statusCahaya = (nilaiCahaya > BATAS_GELAP) ? "GELAP" : "TERANG";
  perbaharuiDashboard(statusCuaca, statusCahaya);

  if (modeOtomatis) {
    int targetBaru;
    if (nilaiHujan < BATAS_HUJAN || nilaiCahaya > BATAS_GELAP) {
      targetBaru = POSISI_DALAM; 
    } else {
      targetBaru = POSISI_LUAR;
    }
    if (motor.targetPosition() != targetBaru) {
      motor.moveTo(targetBaru);
      simpanTarget(targetBaru);
    }
  }
}

// --- Perbarui Status di Dashboard Blynk ---
void perbaharuiDashboard(String cuaca, String cahaya) {
  if (!Blynk.connected()) return;
  if (cuaca != cachedCuaca) { 
    Blynk.virtualWrite(V5, cuaca); 
    cachedCuaca = cuaca; 
  }
  if (cahaya != cachedCahaya) { 
    Blynk.virtualWrite(V6, cahaya); 
    cachedCahaya = cahaya; 
  }
  String statusPosisi = "";
  long jarak = motor.distanceToGo(); 
  long posisi = motor.currentPosition();
  if (jarak == 0) {
    statusPosisi = (posisi <= 100) ? "DALAM" : "LUAR";
  } else {
    statusPosisi = (motor.targetPosition() > posisi) ? "KELUAR >>" : "<< MASUK";
  }
  if (statusPosisi != cachedPosisi) {
    Blynk.virtualWrite(V7, statusPosisi);
    cachedPosisi = statusPosisi;
  }
}

// --- Fungsi Auto-Resume (EEPROM) ---
void simpanDataResume(int target, long posisi) {
  EEPROM.write(ALAMAT_FLAG_RESUME, 1);
  EEPROM.write(ALAMAT_TARGET_RESUME, target);
  EEPROM.put(ALAMAT_POSISI_RESUME, posisi);
  EEPROM.commit();
  Serial.println("[AUTO-RESUME] Data disimpan ke EEPROM");
}

void hapusDataResume() {
  EEPROM.write(ALAMAT_FLAG_RESUME, 0);
  EEPROM.commit();
  Serial.println("[AUTO-RESUME] Data resume dihapus");
}

bool cekPerluResume() {
  return EEPROM.read(ALAMAT_FLAG_RESUME) == 1;
}

void ambilDataResume(int &target, long &posisi) {
  target = EEPROM.read(ALAMAT_TARGET_RESUME);
  EEPROM.get(ALAMAT_POSISI_RESUME, posisi);
}

// --- Jalankan Auto-Resume Jika Perlu (bisa dipanggil di setup & BLYNK_WRITE(V0)) ---
void jalankanAutoResumeJikaPerlu() {
  if (cekPerluResume()) {
    int targetResume;
    long posisiResume;
    ambilDataResume(targetResume, posisiResume);
    Serial.println("[AUTO-RESUME] Melanjutkan ke target terakhir...");
    motor.setCurrentPosition(posisiResume);
    if (targetResume == 0) {
      motor.moveTo(POSISI_DALAM);
    } else {
      motor.moveTo(POSISI_LUAR);
    }
    hapusDataResume();
  }
}

// --- Callback Blynk ---
BLYNK_CONNECTED() {
  Blynk.syncVirtual(V0);
  Blynk.virtualWrite(V1, modeOtomatis);
  Blynk.virtualWrite(V2, modeManual);
  Blynk.virtualWrite(V3, 0);
  Blynk.virtualWrite(V4, 0);
  Serial.println("[IOT] Terhubung ke Blynk Cloud");
}

// --- Tombol Power On/Off ---
BLYNK_WRITE(V0) {
  sistemAktif = param.asInt();
  if(!sistemAktif) {
    modeOtomatis = false;
    modeManual = false;
    simpanMode(); 
    Blynk.virtualWrite(V1, 0);
    Blynk.virtualWrite(V2, 0);
    Serial.println("[CMD] Sistem dimatikan");
    // Simpan data resume jika motor masih bergerak
    if (motor.distanceToGo() != 0) {
      int target = (motor.targetPosition() == POSISI_DALAM) ? 0 : 1;
      simpanDataResume(target, motor.currentPosition());
    } else {
      hapusDataResume();
    }
  } else {
    // --- Tambahan: cek dan jalankan auto-resume jika perlu ---
    jalankanAutoResumeJikaPerlu();
  }
}

// --- Mode Otomatis ---
BLYNK_WRITE(V1) {
  if (!sistemAktif) { 
    Blynk.virtualWrite(V1, 0); 
    return; 
  }
  modeOtomatis = param.asInt();
  if (modeOtomatis) {
    modeManual = false;
    Blynk.virtualWrite(V2, 0);
  } else {
    motor.moveTo(motor.currentPosition());
    simpanTarget(motor.currentPosition());
  }
  simpanMode(); 
}

// --- Mode Manual ---
BLYNK_WRITE(V2) {
  if (!sistemAktif) { 
    Blynk.virtualWrite(V2, 0); 
    return; 
  }
  modeManual = param.asInt();
  if (modeManual) {
    modeOtomatis = false;
    Blynk.virtualWrite(V1, 0);
  }
  simpanMode(); 
}

// --- Paksa Masuk ---
BLYNK_WRITE(V3) {
  int trigger = param.asInt();
  if (trigger != 1 || (millis() - waktuTekanTerakhir < DEBOUNCE_DELAY)) {
    Blynk.virtualWrite(V3, 0);
    return;
  }
  waktuTekanTerakhir = millis();
  if (sistemAktif) {
    modeOtomatis = false;
    modeManual = true;
    simpanMode(); 
    Blynk.virtualWrite(V1, 0);
    Blynk.virtualWrite(V2, 1);
    motor.moveTo(POSISI_DALAM);
    simpanTarget(POSISI_DALAM);
    Blynk.virtualWrite(V3, 0);
    Serial.println("[CMD] Paksa masuk");
  }
}

// --- Paksa Keluar ---
BLYNK_WRITE(V4) {
  int trigger = param.asInt();
  if (trigger != 1 || (millis() - waktuTekanTerakhir < DEBOUNCE_DELAY)) {
    Blynk.virtualWrite(V4, 0);
    return;
  }
  waktuTekanTerakhir = millis();
  if (sistemAktif) {
    modeOtomatis = false;
    modeManual = true;
    simpanMode(); 
    Blynk.virtualWrite(V1, 0);
    Blynk.virtualWrite(V2, 1);
    motor.moveTo(POSISI_LUAR);
    simpanTarget(POSISI_LUAR);
    Blynk.virtualWrite(V4, 0);
    Serial.println("[CMD] Paksa keluar");
  }
}