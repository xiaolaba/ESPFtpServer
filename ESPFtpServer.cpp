/*
 * FTP SERVER FOR ESP32/ESP8266
 * based on FTP Server for Arduino Due and Ethernet shield (W5100) or WIZ820io (W5200)
 * based on Jean-Michel Gallego's work
 * modified to work with esp8266 SPIFFS by David Paiva david@nailbuster.com
 * 2017: modified by @robo8080 (ported to ESP32 and SD)
 * 2019: modified by @fa1ke5 (use SD card in SD_MMC mode (No SD lib, SD_MMC lib), and added fully fuctional passive mode ftp server)
 * 2020: modified by @jmwislez (support generic FS, and re-introduced ESP8266)
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "ESP32FtpServer.h"
#ifdef ESP8266
#include <ESP8266WiFi.h>
#endif
#ifdef ESP32
#include <WiFi.h>
#endif
#include <time.h>


WiFiServer ftpServer (FTP_CTRL_PORT);
WiFiServer dataServer (FTP_DATA_PORT_PASV);

void FtpServer::begin (String uname, String pword) {
  // Tells the ftp server to begin listening for incoming connection
  _FTP_USER = uname;
  _FTP_PASS = pword;

  ftpServer.begin ();
  delay (10);
  dataServer.begin ();
  delay (10);
  millisTimeOut = (uint32_t)FTP_TIME_OUT * 60 * 1000;
  millisDelay = 0;
  cmdStatus = 0;
  iniVariables ();
}

void FtpServer::iniVariables () {
  // Default for data port
  dataPort = FTP_DATA_PORT_PASV;
  
  // Default Data connection is Active
  dataPassiveConn = false;
  
  // Set the root directory
  strcpy (cwdName, "/");

  rnfrCmd = false;
  transferStatus = 0;
}

void FtpServer::handleFTP (fs::FS &fs) {
  if ((int32_t) (millisDelay - millis ()) > 0) {
    return;
  }
  
  if (ftpServer.hasClient ()) {
    #ifdef FTP_DEBUG
    Serial.println ("-> disconnecting client");
    #endif  
    client.stop ();
    client = ftpServer.available ();
  }
  
  if (cmdStatus == 0) {
    if (client.connected ()) {
      disconnectClient ();
    }
    cmdStatus = 1;
  }
  else if (cmdStatus == 1) {         // Ftp server waiting for connection
    abortTransfer ();
    iniVariables ();
    #ifdef FTP_DEBUG
  	Serial.println ("-> ftp server waiting for connection on port " + String (FTP_CTRL_PORT));
    #endif
    cmdStatus = 2;
  }
  else if (cmdStatus == 2) {         // Ftp server idle
    if (client.connected ()) {       // A client connected
      clientConnected ();      
      millisEndConnection = millis () + 10 * 1000; // wait client id during 10 s.
      cmdStatus = 3;
    }
  }
  else if (readChar () > 0) {        // got response
    if (cmdStatus == 3) {            // Ftp server waiting for user identity
      if (userIdentity ()) {
        cmdStatus = 4;
      }
      else {
        cmdStatus = 0;
      }
    }
    else if (cmdStatus == 4) {       // Ftp server waiting for user registration
      if (userPassword ()) {
        cmdStatus = 5;
        millisEndConnection = millis () + millisTimeOut;
      }
      else {
        cmdStatus = 0;
      }
    }
    else if (cmdStatus == 5) {       // Ftp server waiting for user command
      if (!processCommand (fs)) {
        cmdStatus = 0;
      }
      else {
        millisEndConnection = millis () + millisTimeOut;
      }
    }
  }
  else if (!client.connected () || !client) {
	cmdStatus = 1;
    #ifdef FTP_DEBUG
    Serial.println ("-> client disconnected");
    #endif
  }

  if (transferStatus == 1) {         // Retrieve data
    if (!doRetrieve ()) {
      transferStatus = 0;
    }
  }
  else if (transferStatus == 2) {    // Store data
    if (!doStore ()) {
      transferStatus = 0;
    }
  }
  else if (cmdStatus > 2 && ! ((int32_t) (millisEndConnection - millis ()) > 0 )) {
	client.println ("530 Timeout");
    millisDelay = millis () + 200;   // delay of 200 ms
    cmdStatus = 0;
  }
}

void FtpServer::clientConnected () {
  #ifdef FTP_DEBUG
  Serial.println ("-> client connected");
  #endif
  client.println ("220-Welcome to FTP for ESP8266/ESP32");
  client.println ("220-By David Paiva");
  client.println ("220-Version " + String (FTP_SERVER_VERSION));
  client.println ("220 Put your ftp client in passive mode, and do not attempt more than one connection");
  iCL = 0;
}

void FtpServer::disconnectClient () {
  #ifdef FTP_DEBUG
  Serial.println ("-> disconnecting client");
  #endif
  abortTransfer ();
  client.println ("221 Goodbye");
  client.stop ();
}

boolean FtpServer::userIdentity () {	
  if (strcmp (command, "USER")) {
    client.println ("500 Syntax error");
  }
  if (strcmp (parameters, _FTP_USER.c_str ())) {
    client.println ("530 user not found");
  }
  else {
    client.println ("331 OK. Password required");
    strcpy (cwdName, "/");
    return true;
  }
  millisDelay = millis () + 100;  // delay of 100 ms
  return false;
}

boolean FtpServer::userPassword () {
  if (strcmp (command, "PASS")) {
    client.println ("500 Syntax error");
  }
  else if (strcmp (parameters, _FTP_PASS.c_str ())) {
    client.println ("530 ");
  }
  else {
    #ifdef FTP_DEBUG
    Serial.println ("-> user authenticated");
    #endif
    client.println ("230 OK.");
    return true;
  }
  millisDelay = millis () + 100;  // delay of 100 ms
  return false;
}

boolean FtpServer::processCommand (fs::FS &fs) {
  ///////////////////////////////////////
  //                                   //
  //      ACCESS CONTROL COMMANDS      //
  //                                   //
  ///////////////////////////////////////

  //
  //  CDUP - Change to Parent Directory 
  //
  if (!strcmp (command, "CDUP") || (!strcmp (command, "CWD") && !strcmp (parameters, ".."))) {
    bool ok = false;
    if (strlen (cwdName) > 1) {            // do nothing if cwdName is root
      // if cwdName ends with '/', remove it (must not append)
      if (cwdName[strlen (cwdName) - 1] == '/') {
        cwdName[ strlen (cwdName ) - 1 ] = 0;
      }
      // search last '/'
      char * pSep = strrchr (cwdName, '/');
      ok = pSep > cwdName;
      // if found, ends the string on its position
      if (ok) {
        * pSep = 0;
        ok = fs.exists (cwdName);
      }
    }
    // if an error appends, move to root
    if (!ok) {
      strcpy (cwdName, "/");
    }
    client.println ("250 Ok. Current directory is " + String (cwdName));
  }
  
  //
  //  CWD - Change Working Directory
  //
  else if (!strcmp (command, "CWD")) { 
    char path[FTP_CWD_SIZE];
    if (haveParameter () && makeExistsPath (fs, path)) {
      strcpy (cwdName, path);
      client.println ("250 Ok. Current directory is " + String (cwdName));
    }  
  }
  
  //
  //  PWD - Print Directory
  //
  else if (!strcmp (command, "PWD")) {
    client.println ("257 \"" + String (cwdName) + "\" is your current directory");
  }
  
  //
  //  QUIT
  //
  else if (!strcmp (command, "QUIT")) {
    disconnectClient ();
    return false;
  }

  ///////////////////////////////////////
  //                                   //
  //    TRANSFER PARAMETER COMMANDS    //
  //                                   //
  ///////////////////////////////////////

  //
  //  MODE - Transfer Mode 
  //
  else if (!strcmp (command, "MODE")) {
    if (!strcmp (parameters, "S")) {
      client.println ("200 S Ok");
    }
    else {
      client.println ("504 Only S (tream) is supported");
    }
  }
  
  //
  //  PASV - Passive Connection management
  //
  else if (!strcmp (command, "PASV")) {
    if (data.connected ()) {
      data.stop ();
      #ifdef FTP_DEBUG
      Serial.println ("-> client disconnected from dataserver");
      #endif
    }
  	dataIp = WiFi.localIP ();	  
  	dataPort = FTP_DATA_PORT_PASV;
    #ifdef FTP_DEBUG
  	Serial.println ("-> connection management set to passive");
    Serial.println ("-> data port set to " + String (dataPort));
    #endif
    client.println ("227 Entering Passive Mode (" + String (dataIp[0]) + "," + String (dataIp[1]) + "," + String (dataIp[2]) + "," + String (dataIp[3]) + "," + String (dataPort >> 8) + "," + String (dataPort & 255) + ").");
    dataPassiveConn = true;
  }
  
  //
  //  PORT - Data Port
  //
  else if (!strcmp (command, "PORT")) {
    if (data) {
      data.stop ();
      #ifdef FTP_DEBUG
      Serial.println ("-> client disconnected from dataserver");
      #endif
    }
    // get IP of data client
    dataIp[0] = atoi (parameters);
    char * p = strchr (parameters, ',');
    for (uint8_t i = 1; i < 4; i ++) {
      dataIp[i] = atoi (++ p);
      p = strchr (p, ',');
    }
    // get port of data client
    dataPort = 256 * atoi (++ p);
    p = strchr (p, ',');
    dataPort += atoi (++ p);
    if (p == NULL) {
      client.println ("501 Can't interpret parameters");
    }
    else {
      client.println ("200 PORT command successful");
      dataPassiveConn = false;
    }
  }
  
  //
  //  STRU - File Structure
  //
  else if (!strcmp (command, "STRU")) {
    if (!strcmp (parameters, "F")) {
      client.println ("200 F Ok");
    }
    else {
      client.println ("504 Only F (ile) is supported");
    }
  }
  
  //
  //  TYPE - Data Type
  //
  else if (!strcmp (command, "TYPE")) {
    if (!strcmp (parameters, "A")) {
      client.println ("200 TYPE is now ASCII");
    }
    else if (!strcmp (parameters, "I" )) {
      client.println ("200 TYPE is now 8-bit binary");
    }
    else {
      client.println ("504 Unknown TYPE");
    }
  }

  ///////////////////////////////////////
  //                                   //
  //        FTP SERVICE COMMANDS       //
  //                                   //
  ///////////////////////////////////////

  //
  //  ABOR - Abort
  //
  else if (!strcmp (command, "ABOR")) {
    abortTransfer ();
    client.println ("226 Data connection closed");
  }
  
  //
  //  DELE - Delete a File 
  //
  else if (!strcmp (command, "DELE")) {
    char path[FTP_CWD_SIZE];
    if (strlen (parameters) == 0) {
      client.println ("501 No file name");
    }
    else if (makePath (path)) {
      if (!fs.exists (path)) {
        client.println ("550 File " + String (parameters) + " not found");
      }
      else {
        if (fs.remove (path)) {
          client.println ("250 Deleted " + String (parameters));
          // silently recreate the directory if it vanished with the last file it contained
          String directory = String (path).substring (0, String(path).lastIndexOf ("/"));
          if (!fs.exists (directory.c_str())) {
            fs.mkdir (directory.c_str());
          }
        }
        else {
          client.println ("450 Can't delete " + String (parameters));
        }
      }
    }
  }
  
  //
  //  LIST - List 
  //
  else if (!strcmp (command, "LIST")) {
    if (dataConnect ()) {
      client.println ("150 Accepted data connection");
      uint16_t nm = 0;
      char buffer[80];
      
      #ifdef ESP8266
      Dir dir = fs.openDir (cwdName);
      while (dir.next ()) {
        String fname, fsize;
        fname = dir.fileName ();
        time_t ftime = dir.fileTime ();
        struct tm * ptm;
        ptm = gmtime (&ftime);
        int pos = fname.lastIndexOf ("/"); //looking for the beginning of the file by the last "/"
        fname.remove (0, pos + 1); //Delete everything up to and including the filename
        fsize = String (dir.fileSize ());
        if (dir.isDirectory ()){
          sprintf (buffer, "%04d-%02d-%02d  %02d:%02d    <DIR>           %s", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, fname.c_str());
          //data.println ("01/01/2000  00:00    <DIR>           " + fname);
        } 
        else {
          sprintf (buffer, "%04d-%02d-%02d  %02d:%02d    %s  %s", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, fillSpaces (14, String (fsize)).c_str(), fname.c_str());
          //data.println ("01/01/2000  00:00    " + fillSpaces (14, String (fsize)) + "  " + fname);
        }
        data.println (buffer);
        nm ++;
      }
      client.println( "226 " + String (nm) + " matches total");
      #endif
      #ifdef ESP32
      File dir = fs.open (cwdName);
      if ((!dir) || (!dir.isDirectory ())) {
        client.println ("550 Can't open directory " + String (cwdName));
      }
      else {
        File file = dir.openNextFile ();
        while (file) {
          String fname, fsize;
          fname = file.name ();
          time_t ftime = file.getLastWrite ();
          struct tm * ptm;
          ptm = gmtime (&ftime);
          int pos = fname.lastIndexOf ("/"); //looking for the beginning of the file by the last "/"
          fname.remove (0, pos + 1); //Delete everything up to and including the filename
          #ifdef FTP_DEBUG
          Serial.println ("-> " + fname);
          #endif
          fsize = String (file.size ());   
          if (file.isDirectory ()){
          	sprintf (buffer, "%04d-%02d-%02d  %02d:%02d    <DIR>           %s", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, fname);
          } 
          else {
          	sprintf (buffer, "%04d-%02d-%02d  %02d:%02d    %s  %s", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, fillSpaces (14, String (fsize)).c_str(), fname);
          }           
          data.println (buffer);
          nm ++;
          file = dir.openNextFile ();
        }
        client.println ("226 " + String (nm) + " matches total");
      }
      #endif
      data.stop ();
      #ifdef FTP_DEBUG
      Serial.println ("-> client disconnected from dataserver");
      #endif
    }
    else {
      client.println ("425 No data connection");
      data.stop ();
    }    
  }
  
  //
  //  MLSD - Listing for Machine Processing (see RFC 3659)
  //
  else if (!strcmp (command, "MLSD")) {
    if (!dataConnect ()) {
      client.println ("425 No data connection MLSD");
    }
    else {
      client.println ("150 Accepted data connection");
      uint16_t nm = 0;
      #ifdef ESP8266
      Dir dir = fs.openDir (cwdName);
      char dtStr[15];
      while (dir.next ()) {
        String fn, fs;
        fn = dir.fileName ();
        int pos = fn.lastIndexOf ("/"); //looking for the beginning of the file by the last "/"
        fn.remove (0, pos + 1); //Delete everything up to and including the filename
        fs = String (dir.fileSize ());
        if (dir.isDirectory ()) {
          data.println ("Type=dir;Modify=20000101000000; " + fn);
        } 
        else {
          data.println ("Type=file;Size=" + fs + ";"+"modify=20000101000000;" +" " + fn);
        }
        nm ++;
      }
      client.println ("226-options: -a -l");
      client.println("226 " + String(nm) + " matches total");
      #endif
      #ifdef ESP32
      File dir = fs.open (cwdName);
      char dtStr[15];
      if (!fs.exists (cwdName)) {
        client.println ("550 Can't open directory " + String (parameters));
      }
      else {
        File file = dir.openNextFile ();
        while (file) {
          String fn, fs;
          fn = file.name ();
          int pos = fn.lastIndexOf ("/"); // looking for the beginning of the file by the last "/"
          fn.remove (0, pos + 1); // delete everything up to and including the filename
          fs = String (file.size ());
          if (file.isDirectory ()) {
            data.println ("Type=dir;Modify=20000101000000; " + fn);
          } 
          else {
            data.println ("Type=file;Size=" + fs + ";" + "modify=20000101000000;" + " " + fn);
          } 
          nm ++;
          file = dir.openNextFile ();
        }
        client.println ("226-options: -a -l");
        client.println ("226 " + String (nm) + " matches total");
      }
      #endif
      data.stop ();
      #ifdef FTP_DEBUG
      Serial.println ("-> client disconnected from dataserver");
      #endif
    }
  }
  
  //
  //  NLST - Name List 
  //
  else if (!strcmp (command, "NLST" )) {
    if (!dataConnect ()) {
      client.println ("425 No data connection");
    }
    else {
      client.println ("150 Accepted data connection");
      uint16_t nm = 0;
      #ifdef ESP8266
      Dir dir = fs.openDir (cwdName);
      if (!fs.exists (cwdName)) {
        client.println ("550 Can't open directory " + String (parameters));
      }
      else {
        while (dir.next ()) {
          data.println (dir.fileName ());
          nm ++;
        }
        client.println ("226 " + String(nm) + " matches total");
      }
      #endif
      #ifdef ESP32
      File dir = fs.open (cwdName);
      if (!fs.exists (cwdName)) {
        client.println ("550 Can't open directory " + String (parameters));
      }
      else {
        File file = dir.openNextFile ();
        while (file) {
          data.println (file.name ());
          nm ++;
          file = dir.openNextFile ();
        }
        client.println ("226 " + String (nm) + " matches total");
      }
      #endif
      data.stop ();
      #ifdef FTP_DEBUG
      Serial.println ("-> client disconnected from dataserver");
      #endif
    }
  }
  
  //
  //  NOOP
  //
  else if (!strcmp (command, "NOOP")) {
    client.println ("200 Zzz...");
  }
  
  //
  //  RETR - Retrieve
  //
  else if (!strcmp (command, "RETR")) {
    char path[FTP_CWD_SIZE];
    if (strlen (parameters) == 0) {
      client.println ("501 No file name");
    }
    else if (makePath (path)) {
      file = fs.open (path, "r");
      if (!file) {
        client.println ("550 File " + String (parameters) + " not found");
      }
      else if (!file) {
        client.println ("450 Can't open " + String (parameters));
      }
      else if (!dataConnect ()) {
        client.println ("425 No data connection");
      }
      else {
        #ifdef FTP_DEBUG
        Serial.println ("-> sending " + String (parameters));
        #endif
        client.println ("150-Connected to port " + String (dataPort));
        client.println ("150 " + String (file.size ()) + " bytes to download");
        millisBeginTrans = millis ();
        bytesTransferred = 0;
        transferStatus = 1;
      }
    }
  }
  
  //
  //  STOR - Store
  //
  else if (!strcmp (command, "STOR")) {
    char path[FTP_CWD_SIZE];
    if (strlen (parameters) == 0) {
      client.println ("501 No file name");
    }
    else if (makePath (path)) {
	  file = fs.open (path, "w");
      if (!file) {
        client.println ("451 Can't open/create " + String (parameters));
      }
      else if (!dataConnect ()) {
        client.println ("425 No data connection");
        file.close ();
      }
      else {
        #ifdef FTP_DEBUG
        Serial.println ("-> receiving " + String (parameters));
        #endif
        client.println ("150 Connected to port " + String (dataPort));
        millisBeginTrans = millis ();
        bytesTransferred = 0;
        transferStatus = 2;
      }
    }
  }
  
  //
  //  MKD - Make Directory
  //
  
  else if (!strcmp (command, "MKD")) {
    char path[FTP_CWD_SIZE];
    if (haveParameter () && makePath (path)) {
      if (fs.exists (path)) {
        client.println ("521 Can't create \"" + String (parameters) + "\", Directory exists");
      }
      else {
        if (fs.mkdir (path)) {
          client.println ("257 \"" + String (parameters) + "\" created");
        }
        else {
          client.println ("550 Can't create \"" + String (parameters) + "\"");
        }
      }  
    }
  }
  
  //
  //  RMD - Remove a Directory 
  //
  else if (!strcmp (command, "RMD")) {
    char path[FTP_CWD_SIZE];
    if (haveParameter () && makePath (path)) {
      if (fs.rmdir (path)) {
        #ifdef FTP_DEBUG
        Serial.println ("-> deleting " + String (parameters));
        #endif
        client.println ("250 \"" + String (parameters) + "\" deleted");
      }
      else {
      	if (fs.exists (path)) { // hack
          client.println ("550 Can't remove \"" + String (parameters) + "\". Directory not empty?");  
        }
        else {
          #ifdef FTP_DEBUG
          Serial.println ("-> deleting " + String (parameters));
          #endif
          client.println ("250 \"" + String (parameters) + "\" deleted");
        }
      }
    }
  }
  
  //
  //  RNFR - Rename From 
  //
  else if (!strcmp (command, "RNFR")) {
    buf[0] = 0;
    if (strlen (parameters) == 0) {
      client.println ("501 No file name");
    }
    else if (makePath (buf)) {
      if (!fs.exists (buf)) {
        client.println ("550 File " + String (parameters) + " not found");
      }
      else {
        #ifdef FTP_DEBUG
        Serial.println ("-> renaming " + String (buf));
        #endif
        client.println ("350 RNFR accepted - file exists, ready for destination");     
        rnfrCmd = true;
      }
    }
  }
  
  //
  //  RNTO - Rename To 
  //
  else if (!strcmp (command, "RNTO")) {  
    char path[FTP_CWD_SIZE];
    char dir[FTP_FIL_SIZE];
    if (strlen (buf ) == 0 || ! rnfrCmd) {
      client.println ("503 Need RNFR before RNTO");
    }
    else if (strlen (parameters ) == 0) {
      client.println ("501 No file name");
    }
    else if (makePath (path)) {
      if (fs.exists (path)) {
        client.println ("553 " + String (parameters) + " already exists");
      }
      else {          
        #ifdef FTP_DEBUG
        Serial.println ("-> renaming " + String (buf) + " to " + String (path));
        #endif
        if (fs.rename (buf, path)) {
          client.println ("250 File successfully renamed or moved");
        }
        else {
          client.println ("451 Rename/move failure");
        }
      }
    }
    rnfrCmd = false;
  }

  ///////////////////////////////////////
  //                                   //
  //   EXTENSIONS COMMANDS (RFC 3659)  //
  //                                   //
  ///////////////////////////////////////

  //
  //  FEAT - New Features
  //
  else if (!strcmp (command, "FEAT")) {
    client.println ("211-Extensions supported:");
    client.println (" MLSD");
    client.println (" MLST");
    client.println ("211 End.");
  }
  
  //
  //  MDTM - File Modification Time (see RFC 3659)
  //
  else if (!strcmp (command, "MDTM")) {
    client.println ("550 Unable to retrieve time");
  }

  //
  //  SIZE - Size of the file
  //
  else if (!strcmp (command, "SIZE")) {
    char path[FTP_CWD_SIZE];
    if (strlen (parameters) == 0) {
      client.println ("501 No file name");
    }
    else if (makePath (path)) {
      file = fs.open (path, "r");
      if (!file) {
        client.println ("450 Can't open " + String (parameters));
      }
      else {
        client.println ("213 " + String (file.size ()));
        file.close ();
      }
    }
  }
  
  //
  //  MLST - Listing for Machine Processing (see RFC 3659)
  //
  else if (!strcmp (command, "MLST")) {
    char path[FTP_CWD_SIZE];
    if (strlen (parameters) == 0) {
      client.println ("501 No file name");
    }
    else if (makePath (path)) {
      file = fs.open (path, "r");
      if (!file) {
        client.println ("450 Can't open " + String (parameters));
      }
      else {
        client.println ("250-Listing /UPDATES");
        client.println (" Type=file;Size=" + String (file.size ()) + "Modify=20000101010000;create=20000101010000; " + String (file.name ()));
        client.println ("250 End.");
        file.close ();
      }
    }
  }
  
  //
  //  SITE - System command
  //
  else if (!strcmp (command, "SITE")) {
    client.println ("500 Unknown SITE command " + String (parameters));
  }
  
  //
  //  Unrecognized commands ...
  //
  else {
    client.println ("500 Unknown command");
  }
  return true;
}

boolean FtpServer::dataConnect () {
  unsigned long startTime = millis ();
  //wait 5 seconds for a data connection
  if (!data.connected ()) {
    while (!dataServer.hasClient () && millis () - startTime < 10000) {
      yield ();
    }
    if (dataServer.hasClient ()) {
      data.stop ();
      #ifdef FTP_DEBUG
      Serial.println ("-> client disconnected from dataserver");
      #endif
      data = dataServer.available ();
      #ifdef FTP_DEBUG
      Serial.println ("-> client connected to dataserver");
      #endif
    }
  }
  return data.connected ();
}

boolean FtpServer::doRetrieve () {
  if (data.connected ()) {
    int16_t nb = file.readBytes (buf, FTP_BUF_SIZE);
    if (nb > 0) {
      data.write ((uint8_t*)buf, nb);
      bytesTransferred += nb;
      return true;
    }
  }
  closeTransfer ();
  return false;
}


boolean FtpServer::doStore () {
  if (data.connected ()) {
    int16_t nb = data.readBytes ((uint8_t*) buf, FTP_BUF_SIZE);
    if (nb > 0) {
      file.write ((uint8_t*) buf, nb);
      bytesTransferred += nb;
    }
    return true;
  }
  closeTransfer ();
  return false;
}

void FtpServer::closeTransfer () {
  uint32_t deltaT = (int32_t) (millis () - millisBeginTrans);
  if (deltaT > 0 && bytesTransferred > 0) {
    client.println ("226-File successfully transferred");
    client.println ("226 " + String (deltaT) + " ms, " + String (bytesTransferred / deltaT) + " kbytes/s");
  }
  else {
    client.println ("226 File successfully transferred");
  }
  file.close ();
  data.stop ();
  #ifdef FTP_DEBUG
  Serial.println ("-> file successfully transferred");
  Serial.println ("-> client disconnected from dataserver");
  #endif
}

void FtpServer::abortTransfer () {
  if (transferStatus > 0) {
    file.close ();
    data.stop (); 
    #ifdef FTP_DEBUG
    Serial.println ("-> client disconnected from dataserver");
    #endif
    client.println ("426 Transfer aborted");
    #ifdef FTP_DEBUG
    Serial.println ("-> transfer aborted");
    #endif
  }
  transferStatus = 0;
}

// Read a char from client connected to ftp server
//
//  update cmdLine and command buffers, iCL and parameters pointers
//
//  return:
//    -2 if buffer cmdLine is full
//    -1 if line not completed
//     0 if empty line received
//    length of cmdLine (positive) if no empty line received 

int8_t FtpServer::readChar () {
  int8_t rc = -1;

  if (client.available ()) {
    char c = client.read ();
    #ifdef FTP_DEBUG
    Serial.print (c);
    #endif
    if (c == '\\') {
      c = '/';
    }
    if (c != '\r' ) {
      if (c != '\n' ) {
        if (iCL < FTP_CMD_SIZE) {
          cmdLine[iCL ++] = c;
        }
        else {
          rc = -2; //  Line too long
        }
      }
      else {
        cmdLine[iCL] = 0;
        command[0] = 0;
        parameters = NULL;
        // empty line?
        if (iCL == 0) {
          rc = 0;
        }
        else {
          rc = iCL;
          // search for space between command and parameters
          parameters = strchr (cmdLine, ' ');
          if (parameters != NULL) {
            if (parameters - cmdLine > 4) {
              rc = -2; // Syntax error
            }
            else {
              strncpy (command, cmdLine, parameters - cmdLine);
              command[parameters - cmdLine] = 0;
              
              while (* (++ parameters) == ' ') {
                ;
              }
            }
          }
          else if (strlen (cmdLine) > 4) {
            rc = -2; // Syntax error.
          }
          else {
            strcpy (command, cmdLine);
          }
          iCL = 0;
        }
      }
    }
    if (rc > 0) {
      for (uint8_t i = 0; i < strlen (command); i ++) {
        command[i] = toupper (command[i]);
      }
    }
    if (rc == -2) {
      iCL = 0;
      client.println ("500 Syntax error");
    }
  }
  return rc;
}

// Make complete path/name from cwdName and parameters
//
// 3 possible cases: parameters can be absolute path, relative path or only the name
//
// parameters:
//   fullName : where to store the path/name
//
// return:
//    true, if done

boolean FtpServer::makePath (char * fullName) {
  return makePath (fullName, parameters);
}

boolean FtpServer::makePath (char * fullName, char * param) {
  if (param == NULL) {
    param = parameters;
  }
  // Root or empty?
  if (strcmp (param, "/") == 0 || strlen (param) == 0) {
    strcpy (fullName, "/");
    return true;
  }
  // If relative path, concatenate with current dir
  if (param[0] != '/' ) {
    strcpy (fullName, cwdName);
    if (fullName[strlen (fullName) - 1] != '/') {
      strncat (fullName, "/", FTP_CWD_SIZE);
    }
    strncat (fullName, param, FTP_CWD_SIZE);
  }
  else {
    strcpy (fullName, param);
  }
  // If ends with '/', remove it
  uint16_t strl = strlen (fullName) - 1;
  if (fullName[strl] == '/' && strl > 1) {
    fullName[strl] = 0;
  }
  if (strlen (fullName) < FTP_CWD_SIZE) {
    return true;
  }
  client.println ("500 Command line too long");
  return false;
}

// Calculate year, month, day, hour, minute and second
//   from first parameter sent by MDTM command (YYYYMMDDHHMMSS)
//
// parameters:
//   pyear, pmonth, pday, phour, pminute and psecond: pointer of
//     variables where to store data
//
// return:
//    0 if parameter is not YYYYMMDDHHMMSS
//    length of parameter + space

uint8_t FtpServer::getDateTime (uint16_t * pyear, uint8_t * pmonth, uint8_t * pday,
                                uint8_t * phour, uint8_t * pminute, uint8_t * psecond) {
  char dt[15];

  // Date/time are expressed as a 14 digits long string
  // terminated by a space and followed by name of file
  if (strlen (parameters ) < 15 || parameters[14] != ' ') {
    return 0;
  }
  for (uint8_t i = 0; i < 14; i ++) {
    if (!isdigit (parameters[i])) {
      return 0;
    }
  }
  strncpy (dt, parameters, 14);
  dt[14] = 0;
  * psecond = atoi (dt + 12); 
  dt[12] = 0;
  * pminute = atoi (dt + 10);
  dt[10] = 0;
  * phour = atoi (dt + 8);
  dt[8] = 0;
  * pday = atoi (dt + 6);
  dt[6] = 0;
  * pmonth = atoi (dt + 4);
  dt[4] = 0;
  * pyear = atoi (dt);
  return 15;
}

// Create string YYYYMMDDHHMMSS from date and time
//
// parameters:
//    date, time 
//    tstr: where to store the string. Must be at least 15 characters long
//
// return:
//    pointer to tstr

char * FtpServer::makeDateTimeStr (char * tstr, uint16_t date, uint16_t time) {
  sprintf (tstr, "%04u%02u%02u%02u%02u%02u",
           ((date & 0xFE00) >> 9) + 1980, (date & 0x01E0) >> 5, date & 0x001F,
           (time & 0xF800) >> 11, (time & 0x07E0) >> 5, (time & 0x001F) << 1);            
  return tstr;
}

bool FtpServer::haveParameter () {
  if (parameters != NULL && strlen (parameters) > 0) {
    return true;
  }
  client.println ("501 No file name");
  return false;  
}

bool FtpServer::makeExistsPath (fs::FS &fs, char * path, char * param) {
  if (!makePath (path, param)) {
    return false;
  }
  if (fs.exists (path)) {
    return true;
  }
  client.println ("550 " + String (path) + " not found.");
  return false;
}

String FtpServer::fillSpaces (uint8_t length, String input) {
  String output;
  output = "";
  while (output.length() < length - input.length()) {
  	output += " ";
  }
  output += input;
  return (output);
}
