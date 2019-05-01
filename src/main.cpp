#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <FreeRTOS.h>
#include <Arduino.h>

#include <functional>
typedef int (*sqlite_cb_t)(void *, int, char **, char **);

#define takeMuxSemaphore() if( mux ) { xSemaphoreTake(mux, portMAX_DELAY); Serial.println("Took Semaphore"); }
#define giveMuxSemaphore() if( mux ) { xSemaphoreGive(mux); Serial.println("Gave Semaphore"); }

static xSemaphoreHandle mux = NULL;

int rc;
uint32_t _executedTime = 0;
sqlite3 *db1;
static char *zErrMsg = 0;


const char *data = "Callback function called";
static int callback(void *data, int argc, char **argv, char **azColName)
{
  int i;
  Serial.printf("%s: \r\n", (const char *)data);
  for (i = 0; i < argc; i++)
  {
    Serial.printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
  }
  Serial.printf("\n");
  return 0;
}

int openDb(const char *filename, sqlite3 **db)
{
  int rc = sqlite3_open(filename, db);
  if (rc)
  {
    Serial.printf("Can't open database: %s\n", sqlite3_errmsg(*db));
    ESP.deepSleep(1e6);
  }
  else
  {
    Serial.printf("Opened database successfully, status=%d\n", rc);
  }
  return rc;
}

int db_exec(sqlite3 *db, const char *sql, sqlite_cb_t cb = NULL)
{
  takeMuxSemaphore();
  Serial.println(sql);
  long start = micros();
  int rc = SQLITE_ERROR;
  if (cb == NULL)
  {
    rc = sqlite3_exec(db, sql, callback, (void *)data, &zErrMsg);
  }
  else
  {
    rc = sqlite3_exec(db, sql, cb, (void *)data, &zErrMsg);
  }
  _executedTime = (micros() - start) / 1000;
  Serial.printf("Time taken: %lu ms\r\n", _executedTime);
  giveMuxSemaphore();
  return rc;
}


static void NB_IoTTask(void *parameter);

static xQueueHandle xQueue;
// , "CREATE TABLE IF NOT EXISTS datalog (id INTEGER PRIMARY KEY AUTOINCREMENT, date TEXT, time TEXT, heap INTEGER, IDname content, ms INTEGER);");
typedef struct
{
  uint32_t id;
  String dateString;
  String timeString;
  uint32_t voltage_V;
  uint32_t ampere_mA;
  uint32_t heap;
  uint32_t ms;
  String IDString;
} Data_t;

void setupQueue()
{
  xQueue = xQueueCreate(100, sizeof(Data_t));
  mux = xSemaphoreCreateMutex();
  if (xQueue != NULL)
  {
    printf("Queue is created\n");
  }
}

void setupTasks()
{
  xTaskCreatePinnedToCore(NB_IoTTask, "NB_IoTTask", 4096, NULL, 4, NULL, 0);
  static int a;
  a = 4;
  xTaskCreatePinnedToCore([](void *parameter) -> void {
    BaseType_t xStatus;
    const TickType_t xTicksToWait = pdMS_TO_TICKS(100);
    Data_t data;
    data.dateString = "01/05/19";
    data.timeString = "10:43";
    data.voltage_V = 5;
    data.ampere_mA = 100;

    Serial.println("Task Recv is Running..");
    for (;;)
    {
      if (xQueue == NULL)
        continue;
      xStatus = xQueueReceive(xQueue, &data, xTicksToWait);
      static char buffer[100];
      // bzero(_buffer, sizeof(_buffer));
      if (xStatus == pdPASS)
      {
        sprintf(buffer, "INSERT INTO datalog(date, time, voltage, ampere) VALUES('%s', '%s', %lu, %lu);",  data.dateString, data.timeString, data.voltage_V, data.ampere_mA);
        if (db_exec(db1, buffer) == SQLITE_OK)
        {
          Serial.println("INSERT OK.");
        };
      }
    }
  },
                          "insertDBTask", 4096, NULL, 1, NULL, 1);

  xTaskCreatePinnedToCore([](void *parameter) -> void {
    while (1)
    {
      vTaskDelay(10000);
      Serial.println("producerTask is producing...");
      BaseType_t xStatus;
      for (size_t i = 0; i < 100; i++)
      {
        const TickType_t xTicksToWait = pdMS_TO_TICKS(300);
        Data_t data;
        data.ms = millis();
        Serial.println("> sendTask2 is sending data");
        xStatus = xQueueSendToFront(xQueue, &data, xTicksToWait);
        if (xStatus == pdPASS)
        {
          Serial.printf("Queue =%d sent ok.\r\n", i);
        }
        else
        {
          Serial.printf("Queue =%d sent failed.\r\n", i);
        }
      }
    }
  },
                          "producerTask", 4096, NULL, 1, NULL, 1);
}

#define SCK    18
#define MISO   19
#define MOSI   23
#define CS     5

void setup() {
  Serial.begin(115200);
  pinMode(2, INPUT_PULLUP);

  setupQueue();
  setupTasks();

  pinMode(MISO, INPUT_PULLUP);
  SPI.begin(SCK, MISO, MOSI, CS);
  SD.begin();
  sqlite3_initialize();
  delay(50);

  if (openDb("/sd/powerlogger.db", &db1) == SQLITE_OK)
  {
    rc = db_exec(db1, "CREATE TABLE IF NOT EXISTS datalogger (id INTEGER PRIMARY KEY AUTOINCREMENT, date TEXT, time TEXT, name TEXT, voltage INTEGER,  ampere INTEGER);");
    if (rc != SQLITE_OK)
    {
      sqlite3_close(db1);
      return;
    }
  }
}

void loop() {
  taskYIELD();
}

uint32_t currentRowId = 0;
int xcallback(void *data, int argc, char **argv, char **azColName)
{
  currentRowId = strtoul(argv[0] ? argv[0] : "0", NULL, 10);
  Serial.printf("currentRowId = %lu\r\n", currentRowId);
  return 0;
}

static char buffer[100];
static void NB_IoTTask(void *parameter)
{
  while (1)
  {
    sprintf(buffer, "SELECT date,time,voltage,ampere FROM datalogger ORDER BY id DESC LIMIT 1;");
    if (db_exec(db1, buffer, xcallback) == SQLITE_OK)
    {
      Serial.println("QUERY OK.");
    }
    sprintf(buffer, "DELETE FROM datalogger WHERE id = %lu;", currentRowId);
    if (db_exec(db1, buffer) == SQLITE_OK)
    {
      Serial.println("DELETE OK.");
    }
    vTaskDelay(1000);
  }
}