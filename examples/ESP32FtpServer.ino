// Uncomment the file system to use for FTP server

//#define FS_LITTLEFS
//#define FS_SPIFFS
//#define FS_SD_MMC

#include <WiFi.h>
#include <WiFiClient.h>
#include "ESP32FtpServer.h"

#if defined(FS_LITTLEFS)
#include "LITTLEFS.h"
#define FS_ID LITTLEFS
#define FS_NAME "LittleFS"
#elif defined(FS_SPIFFS)
#include "SPIFFS.h"
#define FS_ID SPIFFS
#define FS_NAME "SPIFFS"
#elif defined(FS_SD_MMC)
#include "SD_MMC.h"
#define FS_ID SD_MMC
#define FS_NAME "SD_MMC"
#else 
#define FS_ID SD
#define FS_NAME "UNDEF"
#endif

const char* ssid = "*********************";
const char* password = "*********************";

FtpServer ftpSrv;   //set #define FTP_DEBUG in ESP32FtpServer.h to see ftp verbose on serial

void setup (void) {
  Serial.begin (115200);

  WiFi.begin (ssid, password);
  Serial.println ("");

  // Wait for connection
  while (WiFi.status () != WL_CONNECTED) {
    delay (500);
    Serial.print (".");
  }
  Serial.println ("");
  Serial.print ("Connected to ");
  Serial.println (ssid);
  Serial.print ("IP address: ");
  Serial.println (WiFi.localIP ());

  //FS_ID.format ();
  if (FS_ID.begin ()) {
    Serial.println ("File system opened (" + String (FS_NAME) + ")");
    ftpSrv.begin ("esp32", "esp32");    //username, password for ftp.  set ports in ESP32FtpServer.h  (default 21, 50009 for PASV)
  }
  else {
    Serial.println ("File system could not be opened; ftp server will not work");
  }
}

void loop (void){
  ftpSrv.handleFTP (FS_ID);        //make sure in loop you call handleFTP()!
}
