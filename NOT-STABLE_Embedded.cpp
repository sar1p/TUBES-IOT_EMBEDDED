#define BLYNK_TEMPLATE_ID   "----------"
#define BLYNK_TEMPLATE_NAME "----------"
#define BLYNK_AUTH_TOKEN    "----------"

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <AccelStepper.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_arduino_version.h>

char ssid[] = "-----";
char pass[] = "-----";

#define PIN_MOTOR_IN1 13
#define PIN_MOTOR_IN2 12
#define PIN_MOTOR_IN3 14
#define PIN_MOTOR_IN4 27
#define PIN_HUJAN 34
#define PIN_CAHAYA 35

const int POSISI_DALAM = 0;      
const int POSISI_LUAR  = 4000;   
const int BATAS_HUJAN  = 2800; 
const int BATAS_GELAP  = 3400; 
const int BATAS_SIMPAN = 50; 

#define STACK_MOTOR 4096 
#define STACK_IOT   10240 
#define TIMEOUT_WDT 20

// Kunci Resume
#define KEY_RESUME_FLAG "res_flag"
#define KEY_RESUME_TGT  "res_tgt"
#define KEY_RESUME_POS  "res_pos"

AccelStepper motor(AccelStepper::HALF4WIRE, PIN_MOTOR_IN1, PIN_MOTOR_IN3, PIN_MOTOR_IN2, PIN_MOTOR_IN4);
Preferences memori; 

SemaphoreHandle_t kunciMotor = NULL;
SemaphoreHandle_t kunciData = NULL;
SemaphoreHandle_t kunciMemori = NULL;

volatile bool sistemAktif = true;       
volatile bool modeOtomatis = true; 
volatile bool modeManual = false;  

String cachedCuaca = "";
String cachedCahaya = "";
String cachedPosisi = "";
long cachedPosisiMotor = -9999;
int cachedTarget = -9999;

unsigned long waktuTekanTerakhir = 0;
const unsigned long DEBOUNCE_DELAY = 300;

// Deklarasi
void TaskMotor(void *pvParameters);
void TaskIoT(void *pvParameters);
void jalankanLogika();
void perbaharuiDashboard(String cuaca, String cahaya);
void simpanPosisiOtomatis();
void simpanTarget(int target);
void simpanMode();
void simpanDataResume(int target, long posisi);
void hapusDataResume();
bool cekPerluResume();
void ambilDataResume(int &target, long &posisi);
void jalankanAutoResumeJikaPerlu();

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[BOOT] Memulai Sistem (Anti-Deadlock)...");

  kunciMotor = xSemaphoreCreateMutex();
  kunciData = xSemaphoreCreateMutex();
  kunciMemori = xSemaphoreCreateMutex();

  if (!kunciMotor || !kunciData || !kunciMemori) {
    Serial.println("[FATAL] Gagal buat mutex!");
    while(1) delay(1000);
  }

  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = TIMEOUT_WDT * 1000,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);
  #else
    esp_task_wdt_init(TIMEOUT_WDT, true);
  #endif
  esp_task_wdt_add(NULL);

  int posisiSebelumnya = 0;
  int targetSebelumnya = 0;

  // Gunakan namespace baru "jemuran_v2" untuk reset data korup lama
  if (xSemaphoreTake(kunciMemori, portMAX_DELAY) == pdTRUE) {
    memori.begin("jemuran_v2", false); 
    
    posisiSebelumnya = memori.getInt("posisi", 0); 
    targetSebelumnya = memori.getInt("target", 0);
    modeOtomatis = memori.getBool("modeAuto", true); 
    modeManual = memori.getBool("modeManual", false);
    
    xSemaphoreGive(kunciMemori);
  }

  if (posisiSebelumnya < -100 || posisiSebelumnya > POSISI_LUAR + 1000) posisiSebelumnya = 0;
  if (targetSebelumnya < 0 || targetSebelumnya > POSISI_LUAR) targetSebelumnya = 0;

  motor.setMaxSpeed(1000.0);      
  motor.setAcceleration(1000.0);   
  
  jalankanAutoResumeJikaPerlu();
  
  if (!cekPerluResume()) {
    motor.setCurrentPosition(posisiSebelumnya); 
    motor.moveTo(targetSebelumnya); 
  }

  xTaskCreatePinnedToCore(TaskMotor, "Motor", STACK_MOTOR, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskIoT, "IoT", STACK_IOT, NULL, 1, NULL, 0);
  
  Serial.println("[BOOT] Sistem Siap!");
}

void loop() { 
  vTaskDelete(NULL); 
}

// --- TASK MOTOR (CORE 1) ---
// TUGAS: HANYA GERAK. TIDAK BOLEH AKSES MEMORI (Penyebab Deadlock)
void TaskMotor(void *param) {
  esp_task_wdt_add(NULL); 

  for (;;) {
    esp_task_wdt_reset(); 

    if (xSemaphoreTake(kunciMotor, (TickType_t) 5) == pdTRUE) {
      for(int i=0; i<5; i++) { 
        motor.run(); 
      }
      xSemaphoreGive(kunciMotor); 
    }
    vTaskDelay(1); 
  }
}

// --- TASK IOT (CORE 0) ---
// TUGAS: WIFI, LOGIKA, DAN SIMPAN MEMORI (Background)
void TaskIoT(void *param) {
  esp_task_wdt_add(NULL);
  
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, pass);
  Blynk.config(BLYNK_AUTH_TOKEN); 
  
  unsigned long timerWifi = 0;
  unsigned long timerSimpan = 0;
  int jumlahReconnect = 0;

  for (;;) {
    esp_task_wdt_reset();

    if (WiFi.status() == WL_CONNECTED) {
      if (!Blynk.connected()) Blynk.connect(1000); 
      else Blynk.run();
      jumlahReconnect = 0;
    } else {
      if (millis() - timerWifi > 10000) {
        timerWifi = millis();
        jumlahReconnect++;
        WiFi.reconnect();
        if (jumlahReconnect >= 5) {
          WiFi.disconnect();
          delay(1000);
          WiFi.begin(ssid, pass);
          jumlahReconnect = 0;
        }
      }
    }
    
    jalankanLogika();

    // Simpan posisi dilakukan disini (Aman dari Deadlock)
    if (millis() - timerSimpan > 1000) {
      simpanPosisiOtomatis();
      timerSimpan = millis();
    }

    vTaskDelay(10 / portTICK_PERIOD_MS); 
  }
}

// --- FUNGSI HELPER ---

int bacaSensor(int pin) {
  long total = 0;
  for(int i=0; i<3; i++) { 
    total += analogRead(pin);
    delayMicroseconds(300); 
  }
  return total / 3;
}

void simpanPosisiOtomatis() {
  long posisiSekarang = 0;
  bool motorBerjalan = false;

  // Ambil data motor (Kunci Motor sebentar)
  if (xSemaphoreTake(kunciMotor, pdMS_TO_TICKS(50)) == pdTRUE) {
    posisiSekarang = motor.currentPosition();
    motorBerjalan = motor.isRunning();
    xSemaphoreGive(kunciMotor);
  } else return; 

  // Simpan ke memori (Kunci Memori)
  // Karena dilakukan berurutan (tidak nested), Deadlock tidak mungkin terjadi
  if (abs(posisiSekarang - cachedPosisiMotor) > BATAS_SIMPAN || 
      (!motorBerjalan && posisiSekarang != cachedPosisiMotor)) {
    
    if (xSemaphoreTake(kunciMemori, pdMS_TO_TICKS(50)) == pdTRUE) {
      memori.putInt("posisi", posisiSekarang);
      cachedPosisiMotor = posisiSekarang;
      xSemaphoreGive(kunciMemori);
    }
  }
}

void simpanTarget(int target) {
  if (target != cachedTarget) {
    if (xSemaphoreTake(kunciMemori, pdMS_TO_TICKS(50)) == pdTRUE) {
      memori.putInt("target", target);
      cachedTarget = target;
      xSemaphoreGive(kunciMemori);
    }
  }
}

void simpanMode() {
  if (xSemaphoreTake(kunciMemori, pdMS_TO_TICKS(50)) == pdTRUE) {
    memori.putBool("modeAuto", modeOtomatis);
    memori.putBool("modeManual", modeManual);
    xSemaphoreGive(kunciMemori);
  }
}

// --- AUTO RESUME (PREFERENCES) ---
void simpanDataResume(int target, long posisi) {
  if (xSemaphoreTake(kunciMemori, pdMS_TO_TICKS(50)) == pdTRUE) {
    memori.putBool(KEY_RESUME_FLAG, true);
    memori.putInt(KEY_RESUME_TGT, target);
    memori.putInt(KEY_RESUME_POS, posisi);
    xSemaphoreGive(kunciMemori);
    Serial.println("[RESUME] Data Darurat Disimpan");
  }
}

void hapusDataResume() {
  if (xSemaphoreTake(kunciMemori, pdMS_TO_TICKS(50)) == pdTRUE) {
    memori.putBool(KEY_RESUME_FLAG, false);
    xSemaphoreGive(kunciMemori);
  }
}

bool cekPerluResume() {
  bool perlu = false;
  if (xSemaphoreTake(kunciMemori, pdMS_TO_TICKS(50)) == pdTRUE) {
    perlu = memori.getBool(KEY_RESUME_FLAG, false);
    xSemaphoreGive(kunciMemori);
  }
  return perlu;
}

void ambilDataResume(int &target, long &posisi) {
  if (xSemaphoreTake(kunciMemori, pdMS_TO_TICKS(50)) == pdTRUE) {
    target = memori.getInt(KEY_RESUME_TGT, 0);
    posisi = memori.getInt(KEY_RESUME_POS, 0);
    xSemaphoreGive(kunciMemori);
  }
}

void jalankanAutoResumeJikaPerlu() {
  if (cekPerluResume()) {
    int targetResume;
    long posisiResume;
    ambilDataResume(targetResume, posisiResume);
    
    Serial.println("[RESUME] Melanjutkan tugas...");
    motor.setCurrentPosition(posisiResume);
    
    if (targetResume == 0) motor.moveTo(POSISI_DALAM);
    else motor.moveTo(POSISI_LUAR);
    
    hapusDataResume();
  }
}

// --- LOGIKA UTAMA ---

void jalankanLogika() {
  int nilaiHujan = bacaSensor(PIN_HUJAN);
  int nilaiCahaya = bacaSensor(PIN_CAHAYA);

  bool aktif, otomatis;
  if (xSemaphoreTake(kunciData, pdMS_TO_TICKS(50)) == pdTRUE) {
    aktif = sistemAktif;
    otomatis = modeOtomatis;
    xSemaphoreGive(kunciData);
  } else return;

  if (!aktif) {
    perbaharuiDashboard("OFF", "-");
    if (Blynk.connected()) Blynk.virtualWrite(V7, "SISTEM MATI");
    
    if (xSemaphoreTake(kunciMotor, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (motor.distanceToGo() != 0) {
        motor.moveTo(motor.currentPosition());
        simpanTarget(motor.currentPosition());
      }
      xSemaphoreGive(kunciMotor);
    }
    return;
  }

  String statusCuaca = (nilaiHujan < BATAS_HUJAN) ? "HUJAN" : "KERING";
  String statusCahaya = (nilaiCahaya > BATAS_GELAP) ? "GELAP" : "TERANG";
  perbaharuiDashboard(statusCuaca, statusCahaya);

  if (otomatis) {
    int targetBaru;
    if (nilaiHujan < BATAS_HUJAN || nilaiCahaya > BATAS_GELAP) {
      targetBaru = POSISI_DALAM; 
    } else {
      targetBaru = POSISI_LUAR;
    }
    
    if (xSemaphoreTake(kunciMotor, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (motor.targetPosition() != targetBaru) {
        motor.moveTo(targetBaru);
        simpanTarget(targetBaru);
      }
      xSemaphoreGive(kunciMotor);
    }
  }
}

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
  long jarak = 0;
  long posisi = 0;
  long target = 0;
  
  if (xSemaphoreTake(kunciMotor, pdMS_TO_TICKS(50)) == pdTRUE) {
    jarak = motor.distanceToGo();
    posisi = motor.currentPosition();
    target = motor.targetPosition();
    xSemaphoreGive(kunciMotor);
  }

  if (jarak == 0) {
    if (posisi <= 100) statusPosisi = "DALAM";
    else if (posisi >= POSISI_LUAR - 100) statusPosisi = "LUAR";
    else statusPosisi = "TENGAH";  
  } else {
    statusPosisi = (target > posisi) ? "KELUAR >>" : "<< MASUK";
  }

  if (statusPosisi != cachedPosisi) {
    Blynk.virtualWrite(V7, statusPosisi);
    cachedPosisi = statusPosisi;
  }
}

// --- BLYNK CALLBACKS ---

BLYNK_CONNECTED() {
  Blynk.syncVirtual(V0);
  Blynk.virtualWrite(V1, modeOtomatis);
  Blynk.virtualWrite(V2, modeManual);
  Blynk.virtualWrite(V3, 0);
  Blynk.virtualWrite(V4, 0);
  Serial.println("[IOT] Terhubung");
}

BLYNK_WRITE(V0) {
  if (xSemaphoreTake(kunciData, pdMS_TO_TICKS(50)) == pdTRUE) {
    sistemAktif = param.asInt();
    if(!sistemAktif) {
      modeOtomatis = false;
      modeManual = false;
      
      // Simpan Resume jika motor jalan
      if (xSemaphoreTake(kunciMotor, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (motor.distanceToGo() != 0) {
          int target = (motor.targetPosition() == POSISI_DALAM) ? 0 : 1;
          simpanDataResume(target, motor.currentPosition());
        } else {
          hapusDataResume();
        }
        xSemaphoreGive(kunciMotor);
      }
    } else {
      jalankanAutoResumeJikaPerlu();
    }
    xSemaphoreGive(kunciData);
  }
  simpanMode(); 
  Blynk.virtualWrite(V1, 0);
  Blynk.virtualWrite(V2, 0);
}

BLYNK_WRITE(V1) {
  bool lanjut = false;
  if (xSemaphoreTake(kunciData, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (!sistemAktif) { 
      Blynk.virtualWrite(V1, 0); 
      xSemaphoreGive(kunciData);
      return; 
    }
    modeOtomatis = param.asInt();
    if (modeOtomatis) {
      modeManual = false;
      Blynk.virtualWrite(V2, 0);
    }
    lanjut = !modeOtomatis; 
    xSemaphoreGive(kunciData);
  }
  
  if (lanjut) {
    if (xSemaphoreTake(kunciMotor, pdMS_TO_TICKS(50)) == pdTRUE) {
      motor.moveTo(motor.currentPosition());
      simpanTarget(motor.currentPosition());
      xSemaphoreGive(kunciMotor);
    }
  }
  simpanMode();
}

BLYNK_WRITE(V2) {
  if (xSemaphoreTake(kunciData, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (!sistemAktif) { 
      Blynk.virtualWrite(V2, 0); 
      xSemaphoreGive(kunciData);
      return; 
    }
    modeManual = param.asInt();
    if (modeManual) {
      modeOtomatis = false;
      Blynk.virtualWrite(V1, 0);
    }
    xSemaphoreGive(kunciData);
  }
  simpanMode();
}

BLYNK_WRITE(V3) {
  int trigger = param.asInt();
  if (trigger != 1 || (millis() - waktuTekanTerakhir < DEBOUNCE_DELAY)) {
    Blynk.virtualWrite(V3, 0);
    return;
  }
  waktuTekanTerakhir = millis();
  
  bool aktif = false;
  if (xSemaphoreTake(kunciData, pdMS_TO_TICKS(50)) == pdTRUE) {
    aktif = sistemAktif;
    xSemaphoreGive(kunciData);
  }

  if (aktif) {
    if (xSemaphoreTake(kunciData, pdMS_TO_TICKS(50)) == pdTRUE) {
      modeOtomatis = false;
      modeManual = true;
      xSemaphoreGive(kunciData);
    }
    simpanMode(); 
    Blynk.virtualWrite(V1, 0);
    Blynk.virtualWrite(V2, 1);
    
    if (xSemaphoreTake(kunciMotor, pdMS_TO_TICKS(50)) == pdTRUE) {
      motor.moveTo(POSISI_DALAM);
      simpanTarget(POSISI_DALAM);
      xSemaphoreGive(kunciMotor);
    }
    Blynk.virtualWrite(V3, 0);
  }
}

BLYNK_WRITE(V4) {
  int trigger = param.asInt();
  if (trigger != 1 || (millis() - waktuTekanTerakhir < DEBOUNCE_DELAY)) {
    Blynk.virtualWrite(V4, 0);
    return;
  }
  waktuTekanTerakhir = millis();
  
  bool aktif = false;
  if (xSemaphoreTake(kunciData, pdMS_TO_TICKS(50)) == pdTRUE) {
    aktif = sistemAktif;
    xSemaphoreGive(kunciData);
  }

  if (aktif) {
    if (xSemaphoreTake(kunciData, pdMS_TO_TICKS(50)) == pdTRUE) {
      modeOtomatis = false;
      modeManual = true;
      xSemaphoreGive(kunciData);
    }
    simpanMode(); 
    Blynk.virtualWrite(V1, 0);
    Blynk.virtualWrite(V2, 1);
    
    if (xSemaphoreTake(kunciMotor, pdMS_TO_TICKS(50)) == pdTRUE) {
      motor.moveTo(POSISI_LUAR);
      simpanTarget(POSISI_LUAR);
      xSemaphoreGive(kunciMotor);
    }
    Blynk.virtualWrite(V4, 0);
  }
}