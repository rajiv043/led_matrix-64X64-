#include <Arduino.h>
#include <BluetoothSerial.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <LittleFS.h>
#include <Fonts/FreeSansBold9pt7b.h>
// #include <esp_task_wdt.h>

#define PANEL_WIDTH 64
#define PANEL_HEIGHT 64
#define IMAGE_SIZE (PANEL_WIDTH * PANEL_HEIGHT * 2) // 8192 bytes (64x64x2)
#define GIF_FRAME_SIZE IMAGE_SIZE
#define BUFFER_SIZE 32768 // 32KB buffer
#define MAX_FRAMES 32

// Forward declarations
void processImageData();
void displayBufferedImage();

MatrixPanel_I2S_DMA *matrix;
HUB75_I2S_CFG config(PANEL_WIDTH, PANEL_HEIGHT, 1);

BluetoothSerial SerialBT;
uint16_t imageBuffer[PANEL_WIDTH][PANEL_HEIGHT] = {0};
uint8_t imageData[IMAGE_SIZE];
uint8_t fileBuffer[BUFFER_SIZE]; // 32KB buffer for file operations
size_t receivedBytes = 0;

struct FileInfo {
    uint16_t id;
    uint16_t numFrames;
    bool isGIF;
};

FileInfo currentFile = {0, 0, false};
bool isRunning = false;

int16_t getTextWidth(const char* text) {
    int16_t x1, y1;
    uint16_t w, h;
    matrix->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    return w;
}

void setup() {
    Serial.begin(115200);
    SerialBT.begin("ESP32_MATRIX");
    Serial.println("Bluetooth Ready. Waiting for commands...");

    if (!LittleFS.begin(true)) {
        Serial.println("Failed to mount LittleFS");
        return;
    }
    Serial.println("LittleFS mounted successfully");
    // esp_task_wdt_init(5, true);
    // esp_task_wdt_add(NULL);

    // HUB75 GPIO Configuration
    config.gpio.e   = 32;
    config.gpio.d   = 17;
    config.gpio.c   = 5;
    config.gpio.b = 22;
    config.gpio.a = 23;
    config.gpio.lat = 4;
    config.gpio.oe = 15;
    config.gpio.clk = 18;
    config.gpio.r1 = 25;
    config.gpio.g1 = 26;
    config.gpio.b1 = 27;
    config.gpio.r2 = 14;
    config.gpio.g2 = 12;
    config.gpio.b2 = 13;

    matrix = new MatrixPanel_I2S_DMA(config);
    matrix->begin();
    matrix->fillScreen(matrix->color565(0, 0, 0));
    matrix->setTextSize(1);
    matrix->setFont(&FreeSansBold9pt7b);
    matrix->setTextColor(matrix->color565(255, 255, 255));
}

void loop() {
    // esp_task_wdt_reset();
    if (SerialBT.available()) {
        char command = SerialBT.read();
        switch (command) {
            case 'U': // Upload new file (GIF or image)
                uploadFile();
                break;
            case 'R': // Run stored file or text
                runStoredFile();
                break;
            case 'D': // Delete stored file or text
                deleteFile();
                break;
            case 'T': // Display text
                handleTextCommand();
                break;
            default:
                Serial.println("Unknown command");
                break;
        }
    }
}

void handleTextCommand() {
    uint16_t fileID;
    SerialBT.readBytes((uint8_t*)&fileID, 2);

    // Read text length (2 bytes)
    uint16_t textLength;
    SerialBT.readBytes((uint8_t*)&textLength, 2);

    // Read text data
    char text[textLength + 1];
    SerialBT.readBytes((uint8_t*)text, textLength);
    text[textLength] = '\0';

    // Store the text in LittleFS
    storeText(fileID, text);

    // Display the text
    displayText(fileID);
}

void uploadFile() {
    // Read file ID (2 bytes)
    uint16_t fileID;
    SerialBT.readBytes((uint8_t*)&fileID, 2);
    Serial.println("Uploading file with ID: " + String(fileID));

    // Read file type (1 byte: 0 for image, 1 for GIF)
    uint8_t fileType;
    SerialBT.readBytes(&fileType, 1);
    bool isGIF = (fileType == 1);
    Serial.println("File type: " + String(isGIF ? "GIF" : "Image"));

    // Read number of frames (2 bytes)
    uint16_t numFrames;
    SerialBT.readBytes((uint8_t*)&numFrames, 2);
    Serial.println("Number of frames: " + String(numFrames));

    // Calculate total size and create filename
    size_t totalSize = isGIF ? (numFrames * GIF_FRAME_SIZE) : GIF_FRAME_SIZE;
    String filename = "/file_" + String(fileID) + ".bin";

    // Remove existing file if it exists
    if (LittleFS.exists(filename)) {
        LittleFS.remove(filename);
    }

    // Open file for writing
    File file = LittleFS.open(filename, "wb");
    if (!file) {
        Serial.println("Failed to create file: " + filename);
        return;
    }

    // Save file info
    String infoFilename = "/file_" + String(fileID) + "_info.bin";
    File infoFile = LittleFS.open(infoFilename, "wb");
    if (infoFile) {
        infoFile.write((uint8_t*)&numFrames, sizeof(numFrames));
        infoFile.write(&fileType, sizeof(fileType));
        infoFile.close();
    }

    // Receive and store data
    size_t receivedTotal = 0;
    size_t bufferIndex = 0;
    unsigned long lastDataTime = millis();

    while (receivedTotal < totalSize) {
        size_t remaining = totalSize - receivedTotal;
        size_t toRead = min((size_t)(BUFFER_SIZE - bufferIndex), remaining);

        size_t actualRead = SerialBT.readBytes(fileBuffer + bufferIndex, toRead);
        if (actualRead == 0) {
            if (millis() - lastDataTime > 5000) {
                break; // Timeout
            }
            delay(10);
            continue;
        }

        bufferIndex += actualRead;
        receivedTotal += actualRead;
        lastDataTime = millis();

        // Write buffer to file if it's full or we've received all data
        if (bufferIndex == BUFFER_SIZE || receivedTotal == totalSize) {
            file.write(fileBuffer, bufferIndex);
            bufferIndex = 0;
            SerialBT.write('A'); // Send acknowledgment
        }
    }

    file.close();
    Serial.println("File upload complete: " + filename);

    // Automatically run the uploaded file
    runFile(fileID);
}

void runFile(uint16_t fileID) {
    // Load file info
    String infoFilename = "/file_" + String(fileID) + "_info.bin";
    File infoFile = LittleFS.open(infoFilename, "rb");
    if (!infoFile) {
        Serial.println("Failed to open file info: " + infoFilename);
        return;
    }

    uint16_t numFrames;
    infoFile.read((uint8_t*)&numFrames, sizeof(numFrames));
    uint8_t fileType;
    infoFile.read(&fileType, sizeof(fileType));
    infoFile.close();

    bool isGIF = (fileType == 1);

    // Stop any currently running file
    if (isRunning) {
        isRunning = false;
        delay(100);
    }

    // Set current file
    currentFile.id = fileID;
    currentFile.numFrames = numFrames;
    currentFile.isGIF = isGIF;
    isRunning = true;

    // Open the data file
    String filename = "/file_" + String(fileID) + ".bin";
    File file = LittleFS.open(filename, "rb");
    if (!file) {
        Serial.println("Failed to open file: " + filename);
        return;
    }

    // Display frames
    while (isRunning) {
        for (uint16_t frame = 0; frame < currentFile.numFrames; frame++) {
            if (!file.seek(frame * GIF_FRAME_SIZE)) {
                Serial.println("Seek failed for frame: " + String(frame));
                break;
            }

            size_t bytesRead = file.read(imageData, GIF_FRAME_SIZE);
            if (bytesRead == GIF_FRAME_SIZE) {
                processImageData();
                displayBufferedImage();
            } else {
                Serial.println("Failed to read frame: " + String(frame));
            }

            if (!currentFile.isGIF) {
                isRunning = false;
                break;
            }
            delay(1); // Adjust for GIF frame rate
        }
        
        if (currentFile.isGIF) {
            file.seek(0); // Rewind for next loop
        }
    }

    file.close();
}

// [Rest of the functions remain the same as your original code]
// runStoredFile(), deleteFile(), stopFile(), storeText(), displayText(), 
// processImageData(), displayBufferedImage() remain unchanged

// [Rest of the functions remain the same as your original code]
// runStoredFile(), deleteFile(), stopFile(), storeText(), displayText(), 
// processImageData(), displayBufferedImage() remain unchanged

void runStoredFile() {
    // Read file ID (2 bytes)
    uint16_t fileID;
    SerialBT.readBytes((uint8_t*)&fileID, 2);
    Serial.println("Running stored file with ID: " + String(fileID));

    // Check if it's a text file
    String textFilename = "/text_" + String(fileID) + ".txt";
    if (LittleFS.exists(textFilename)) {
        displayText(fileID);
    } else {
        runFile(fileID);
    }
}

void deleteFile() {
    // Read file ID (2 bytes)
    uint16_t fileID;
    SerialBT.readBytes((uint8_t*)&fileID, 2);
    Serial.println("Deleting file with ID: " + String(fileID));

    // Stop the file if it is currently running
    if (isRunning && currentFile.id == fileID) {
        isRunning = false;
        delay(100); // Allow the running loop to exit
    }

    // Delete all frames and info file
    String infoFilename = "/file_" + String(fileID) + "_info.bin";
    if (LittleFS.remove(infoFilename)) {
        Serial.println("File info deleted: " + infoFilename);
    } else {
        Serial.println("Failed to delete file info: " + infoFilename);
    }

    for (uint16_t frame = 0; frame < 100; frame++) { // Assume max 100 frames per file
        String frameFilename = "/file_" + String(fileID) + "frame" + String(frame) + ".bin";
        if (LittleFS.remove(frameFilename)) {
            Serial.println("Frame deleted: " + frameFilename);
        } else {
            break; // No more frames to delete
        }
    }

    // Delete text file if it exists
    String textFilename = "/text_" + String(fileID) + ".txt";
    if (LittleFS.remove(textFilename)) {
        Serial.println("Text file deleted: " + textFilename);
    }

    // Send acknowledgment
    SerialBT.write('A');
}

void stopFile() {
    if (isRunning) {
        isRunning = false;
        matrix->fillScreen(matrix->color565(0, 0, 0)); // Clear the screen
        Serial.println("Stopped running file");
    }
    SerialBT.write('A'); // Send acknowledgment
}
void storeText(uint16_t fileID, const char *text) {
    // Create a filename for the text file
    String textFilename = "/text_" + String(fileID) + ".txt";

    // Open the file for writing
    File textFile = LittleFS.open(textFilename, "wb");
    if (!textFile) {
        Serial.println("Failed to create text file: " + textFilename);
        return;
    }

    // Write the text to the file
    size_t bytesWritten = textFile.write((const uint8_t *)text, strlen(text));
    textFile.close();

    if (bytesWritten == strlen(text)) {
        Serial.println("Text stored successfully: " + textFilename);
    } else {
        Serial.println("Failed to store text: " + textFilename);
    }
}

void displayText(uint16_t fileID) {
    String textFilename = "/text_" + String(fileID) + ".txt";
    File textFile = LittleFS.open(textFilename, "rb");
    if (!textFile) {
        Serial.println("Failed to open text file: " + textFilename);
        return;
    }

    // Read text data
    size_t textLength = textFile.size();
    char text[textLength + 1];
    textFile.read((uint8_t*)text, textLength);
    text[textLength] = '\0'; // Null-terminate the string
    textFile.close();
    Serial.println("Displaying text: " + String(text));

    // Clear the screen
    matrix->fillScreen(matrix->color565(0, 0, 0));

    // Display text with word wrap
    int16_t x = 0, y = 16; // Start at top-left with 16px font height
    char *word = strtok(text, " ");
    while (word) {
        int16_t wordWidth = getTextWidth(word);
        if (x + wordWidth > PANEL_WIDTH) {
            x = 0;
            y += 16; // Move to the next line
        }
        matrix->setCursor(x, y);
        matrix->print(word);
        x += wordWidth + 6; // Add space between words
        word = strtok(NULL, " ");
    }

    // Send acknowledgment
    SerialBT.write('A');
}

void processImageData() {
    for (int i = 0; i < PANEL_WIDTH * PANEL_HEIGHT; i++) {
        int x = i % PANEL_WIDTH;
        int y = i / PANEL_WIDTH;
        uint16_t color = (imageData[i * 2 + 1] << 8) | imageData[i * 2];
        imageBuffer[x][y] = color;
    }
}

void displayBufferedImage() {
    for (int x = 0; x < PANEL_WIDTH; x++) {
        for (int y = 0; y < PANEL_HEIGHT; y++) {
            matrix->drawPixel(x, y, imageBuffer[x][y]);
        }
    }
}
