#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

#define PIN_TRIG     12
#define PIN_ECHO     13
#define PIN_BUTTON   25
#define PIN_SERVO    26
#define PIN_LED      27
#define PIN_BUZZER   14

#define LEVEL_SIAGA  50
#define LEVEL_BAHAYA 80

#define FREQ_SIAGA   1000
#define FREQ_BAHAYA  2500
#define FREQ_DARURAT 3000

LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo servoPintu;

//  FREERTOS
QueueHandle_t     queueAir;
SemaphoreHandle_t mutexLCD;
TaskHandle_t      TaskButton_Handle = NULL;

volatile bool manualMode = false;

// FUNGSI BACA HC-SR04
int bacaJarak() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long duration = pulseIn(PIN_ECHO, HIGH, 25000);
  if (duration == 0) return 400;
  return constrain((int)(duration * 0.034 / 2), 0, 400);
}

// ISR — TOMBOL
void IRAM_ATTR isrTombol() {
  BaseType_t xTaskWoken = pdFALSE;
  vTaskNotifyGiveFromISR(TaskButton_Handle, &xTaskWoken);
  if (xTaskWoken) portYIELD_FROM_ISR();
}

// TASK 1: SENSOR — Prioritas 2, Periode 100ms
// 0cm = 100% air | 400cm = 0% air | 200cm = 50%
void Task_Sensor(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(100);

  for (;;) {
    int jarak     = bacaJarak();
    int persenAir = map(jarak, 400, 0, 0, 100);
    persenAir     = constrain(persenAir, 0, 100);

    xQueueOverwrite(queueAir, &persenAir);

    UBaseType_t sisaRAM = uxTaskGetStackHighWaterMark(NULL);

    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

// TASK 2: SERVO — Prioritas 3, Event-driven
void Task_Servo(void *pvParameters) {
  int dataAir = 0;
  for (;;) {
    xQueuePeek(queueAir, &dataAir, portMAX_DELAY);

    if (!manualMode) {
      int sudutServo;
      if      (dataAir >= LEVEL_BAHAYA) sudutServo = 90;
      else if (dataAir >= LEVEL_SIAGA)  sudutServo = 60;
      else                               sudutServo = map(dataAir, 0, LEVEL_SIAGA, 0, 45);
      servoPintu.write(sudutServo);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// TASK 3: LCD — Prioritas 1, Periode 300ms
void Task_LCD(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(300);
  int dataAir = 0;

  for (;;) {
    if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(200)) == pdTRUE) {
      xQueuePeek(queueAir, &dataAir, 0);

      if (manualMode) {
        lcd.setCursor(0, 0);
        lcd.print("!! DARURAT !!   ");
        lcd.setCursor(0, 1);
        lcd.print("Pintu TERBUKA   ");
      } else {
        lcd.setCursor(0, 0);
        if      (dataAir >= LEVEL_BAHAYA) lcd.print("Status: BAHAYA  ");
        else if (dataAir >= LEVEL_SIAGA)  lcd.print("Status: SIAGA   ");
        else                               lcd.print("Status: AMAN    ");

        lcd.setCursor(0, 1);
        lcd.print("Debit Air: ");
        lcd.print(dataAir);
        lcd.print("%  ");
      }

      xSemaphoreGive(mutexLCD);
    }
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

// TASK 4: LED + BUZZER — Prioritas 2, Periode 200ms
void Task_LED(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(200);
  int dataAir  = 0;
  bool ledState = false;

  for (;;) {
    xQueuePeek(queueAir, &dataAir, 0);

    if (!manualMode) {
      if (dataAir >= LEVEL_BAHAYA) {
        digitalWrite(PIN_LED, HIGH);
        tone(PIN_BUZZER, FREQ_BAHAYA);

      } else if (dataAir >= LEVEL_SIAGA) {
        ledState = !ledState;
        digitalWrite(PIN_LED, ledState);
        if (ledState) {
          tone(PIN_BUZZER, FREQ_SIAGA);
          vTaskDelay(pdMS_TO_TICKS(50));
          noTone(PIN_BUZZER);
        }

      } else {
        digitalWrite(PIN_LED, LOW);
        noTone(PIN_BUZZER);
      }
    }

    UBaseType_t sisaRAM = uxTaskGetStackHighWaterMark(NULL);
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

// TASK 5: BUTTON — Prioritas 4 (TERTINGGI)
void Task_Button(void *pvParameters) {
  TickType_t lastPressTime = 0;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Anti-bounce
    TickType_t now = xTaskGetTickCount();
    if ((now - lastPressTime) < pdMS_TO_TICKS(300)) continue;
    lastPressTime = now;

    vTaskDelay(pdMS_TO_TICKS(100));

    if (!manualMode) {
      //  TEKAN 1: DARURAT — hardware langsung 
      manualMode = true;
      servoPintu.write(90);          // langsung, tidak tunggu Task_Servo
      digitalWrite(PIN_LED, HIGH);
      tone(PIN_BUZZER, FREQ_DARURAT);

      if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(500)) == pdTRUE) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("!! DARURAT !!");
        lcd.setCursor(0, 1);
        lcd.print("Pintu TERBUKA");
        xSemaphoreGive(mutexLCD);
      }

    } else {
      //  TEKAN 2: balik otomatis 
      manualMode = false;
      noTone(PIN_BUZZER);
      digitalWrite(PIN_LED, LOW);
      servoPintu.write(0);           // tutup dulu, biar Task_Servo ambil alih

      if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(500)) == pdTRUE) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(">> AUTO MODE");
        lcd.setCursor(0, 1);
        lcd.print("Sensor aktif");
        xSemaphoreGive(mutexLCD);
      }
    }
  }
}

// SETUP
void setup() {
  Serial.begin(115200);

  pinMode(PIN_TRIG,   OUTPUT);
  pinMode(PIN_ECHO,   INPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED,    OUTPUT);
  servoPintu.attach(PIN_SERVO);
  servoPintu.write(0);

  Wire.begin();    
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Sistem Pemantau dan");
  lcd.setCursor(0, 1);
  lcd.print("Pengendali air");
  delay(1500);
  lcd.clear();

  queueAir = xQueueCreate(1, sizeof(int));
  mutexLCD = xSemaphoreCreateMutex();

  if (queueAir == NULL || mutexLCD == NULL) {
    lcd.setCursor(0, 0);
    lcd.print("ERROR: RTOS INIT");
    while (1);
  }

  // PRIORITAS RMS:
  // Sensor  100ms → Prioritas 2
  // LED     200ms → Prioritas 2
  // LCD     300ms → Prioritas 1
  // Servo   event → Prioritas 3
  // Button  event → Prioritas 4 (dikecualikan RMS)
  //
  // U = 20/100 + 20/200 + 30/300 = 0.40
  // Batas RMS n=3 = 0.780
  // 0.40 < 0.780 -> SCHEDULABLE

  xTaskCreate(Task_LCD,    "TaskLCD",    2048, NULL, 1, NULL);
  xTaskCreate(Task_Sensor, "TaskSensor", 2048, NULL, 2, NULL);
  xTaskCreate(Task_LED,    "TaskLED",    2048, NULL, 2, NULL);
  xTaskCreate(Task_Servo,  "TaskServo",  2048, NULL, 3, NULL);
  xTaskCreate(Task_Button, "TaskButton", 2048, NULL, 4, &TaskButton_Handle);

  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), isrTombol, FALLING);
}

void loop() {}
