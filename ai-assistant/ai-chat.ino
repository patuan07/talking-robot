#include <WiFi.h>
#include <ArduinoASRChat.h>
#include <ArduinoGPTChat.h>
#include <Audio.h>

// Enable conversation memory (set to 0 to disable)
#define ENABLE_CONVERSATION_MEMORY 1

// Define I2S pins for audio output (speaker)
#define I2S_DOUT 47
#define I2S_BCLK 48
#define I2S_LRC 45

// Define INMP441 microphone input pins
// INMP441 wiring:
// VDD -> 3.3V (DO NOT use 5V!)
// GND -> GND
// L/R -> GND (select left channel)
// WS  -> GPIO 4 (left/right clock)
// SCK -> GPIO 5 (serial clock)
// SD  -> GPIO 6 (serial data)
#define I2S_MIC_SERIAL_CLOCK 5   // SCK - serial clock
#define I2S_MIC_LEFT_RIGHT_CLOCK 4 // WS - left/right clock
#define I2S_MIC_SERIAL_DATA 6     // SD - serial data

// Define boot button pin (GPIO0 is the boot button on most ESP32 boards)
//#define BOOT_BUTTON_PIN 0

// Sample rate for recording
#define SAMPLE_RATE 16000


int GreenPin = 17;
int YellowPin = 16;
int RedPin = 15;

// WiFi settings
const char* ssid     = "Tai";
const char* password = "Tai12345";

// ByteDance ASR API configuration
const char* asr_api_key = "BYTE-DANCE-API-KEY";
const char* asr_cluster = "volcengine_input_en";

// OpenAI API configuration for LLM and TTS
const char* openai_apiKey = "MY_OPEN_API_KEY";
const char* openai_apiBaseUrl = "https://api.openai.com";

// OPENAI TTS INITIALIZATION
const String OPENAI_API_KEY = "MY_OPEN_API_KEY";
const String tts_model = "tts-1";
const String instruction = "";
const String voice = "alloy";
const String format = "mp3";
const String speed = "1";

// Previous prompt (Spark Buddy)

const char* systemPrompt = "You are Ben, a witty, warm chat companion. "
"Goal: make any topic fun and insightful. "
"Style: concise, lively; avoid corporate tone and emoji. "
"Behavior: "
"- Start with a one-sentence takeaway, then add 1-3 fun, actionable tips or ideas. "
"- Ask at most 1 precise question to move the chat. "
"- If unsure, say so and offer safe next steps. "
"- Don't fabricate facts/data/links; avoid fluff and repetition. "
"- Add light games/analogies/micro-challenges for fun. "
"Compression: Keep each reply <=20 words when possible.";

// ===== WAKE PHRASE CONFIGURATION =====
const char* WAKE_PHRASES[] = {
  "hey, ben",
  "hi, ben",
  "hello, ben",
  "ben"
};

const int NUM_WAKE_PHRASES = sizeof(WAKE_PHRASES) / sizeof(WAKE_PHRASES[0]);


// Global audio variable for TTS playback
Audio audio;

// Initialize ASR and GPT Chat instances
ArduinoASRChat asrChat(asr_api_key, asr_cluster);
ArduinoGPTChat gptChat(openai_apiKey, openai_apiBaseUrl);

// Continuous conversation mode state machine
enum ConversationState {
  STATE_IDLE,              // Waiting for button press to start
  STATE_LISTENING,         // ASR is recording and listening
  STATE_PROCESSING_LLM,    // Processing with ChatGPT
  STATE_PLAYING_TTS,       // TTS is playing
  STATE_WAIT_TTS_COMPLETE  // Waiting for TTS to complete
};

// State variables
ConversationState currentState = STATE_IDLE;
bool continuousMode = false;
//bool buttonPressed = false;
//bool wasButtonPressed = false;
unsigned long ttsStartTime = 0;
unsigned long ttsCheckTime = 0;

void setup() {
  // Initialize serial port
  Serial.begin(115200);
  delay(1000);

  pinMode(GreenPin, OUTPUT);
  pinMode(YellowPin, OUTPUT);
  pinMode(RedPin, OUTPUT);

  digitalWrite(GreenPin, LOW);
  digitalWrite(YellowPin, LOW);
  digitalWrite(RedPin, HIGH);

  Serial.println("\n\n----- Voice Assistant System (ASR+LLM+TTS) Starting -----");

  // Initialize random seed
  randomSeed(analogRead(0) + millis());

  // Connect to WiFi network
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi...");

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 20) {
    Serial.print('.');
    delay(1000);
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected successfully!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Free Heap: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");

    // Set I2S output pins for TTS playback
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    // Set volume
    audio.setVolume(100);

    // Set system prompt for GPT
    gptChat.setSystemPrompt(systemPrompt);

    // Enable conversation memory if configured
#if ENABLE_CONVERSATION_MEMORY
    gptChat.enableMemory(true);
    Serial.println("Conversation memory: ENABLED");
#else
    gptChat.enableMemory(false);
    Serial.println("Conversation memory: DISABLED");
#endif

    // Initialize INMP441 microphone for ASR
    if (!asrChat.initINMP441Microphone(I2S_MIC_SERIAL_CLOCK, I2S_MIC_LEFT_RIGHT_CLOCK, I2S_MIC_SERIAL_DATA)) {
      Serial.println("Failed to initialize microphone!");
      return;
    }

    // Set audio parameters for ASR
    asrChat.setAudioParams(SAMPLE_RATE, 16, 1);
    asrChat.setSilenceDuration(1000);  // 1 second silence detection
    asrChat.setMaxRecordingSeconds(30);

    // Set timeout no speech callback - exit continuous mode if timeout without speech
    asrChat.setTimeoutNoSpeechCallback([]() {
      if (continuousMode) {
        stopContinuousMode();
      }
    });

    // Connect to ByteDance ASR WebSocket
    if (!asrChat.connectWebSocket()) {
      Serial.println("Failed to connect to ASR service!");
      return;
    }

    stopContinuousMode();

  } else {
    Serial.println("\nFailed to connect to WiFi. Please check network credentials and retry.");
  }
}

bool containsWakePhrase(const String& text) {
  String lower = text;
  lower.toLowerCase();

  for (int i = 0; i < NUM_WAKE_PHRASES; i++) {
    if (lower.indexOf(WAKE_PHRASES[i]) >= 0) {
      return true;
    }
  }
  return false;
}

void startContinuousMode() {
  continuousMode = true;
  currentState = STATE_LISTENING;
  asrChat.startRecording();
}

void stopContinuousMode() {
  continuousMode = false;

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║  Continuous Conversation Mode Stopped ║");
  Serial.println("╚════════════════════════════════════════╝");

  currentState = STATE_IDLE;
  asrChat.startRecording();
  Serial.println("[ASR] Passive listening for wake phrase...");
}

void handleASRResult() {
  String transcribedText = asrChat.getRecognizedText();
  asrChat.clearResult();

  if (transcribedText.length() == 0) {
    asrChat.startRecording();
    return;
  }

  Serial.printf("[ASR] %s\n", transcribedText.c_str());

  // ===== WAKE PHRASE CHECK =====
  if (!continuousMode) {
    if (containsWakePhrase(transcribedText)) {
      Serial.println("[WAKE] Wake phrase detected");
      audio.openai_speech(OPENAI_API_KEY, tts_model, "How can I help you today", voice, format, speed);
      startContinuousMode();
    } else {
      // Ignore everything else
      asrChat.startRecording();
    }
    return;
  }

  Serial.println("\n╔═══ ASR Recognition Result ═══╗");
  Serial.printf("║ %s\n", transcribedText.c_str());
  Serial.println("╚══════════════════════════════╝");

  currentState = STATE_PROCESSING_LLM;
  Serial.println("\n[LLM] Sending to ChatGPT...");

  const String response = gptChat.sendMessage(transcribedText);

  if (response.length() == 0) {
    Serial.println("[ERROR] Failed to get ChatGPT response");

    if (continuousMode) {
      currentState = STATE_LISTENING;
      asrChat.startRecording();
    } else {
      continuousMode = false;
      currentState = STATE_IDLE;
      asrChat.startRecording();
    }
    return;
  }

  Serial.println("\n╔═══ ChatGPT Response ═══╗");
  Serial.printf("║ %s\n", response.c_str());
  Serial.println("╚════════════════════════╝");

  Serial.println("\n[TTS] Converting to speech and playing...");

  //audio.connecttospeech(response.c_str(), "en");
  audio.openai_speech(OPENAI_API_KEY, tts_model, response, voice, format, speed);

  currentState = STATE_WAIT_TTS_COMPLETE;
  ttsStartTime = millis();
  ttsCheckTime = millis();
}

void loop() {
  // Handle audio loop (TTS playback)
  audio.loop();

  // Handle ASR processing
  asrChat.loop();

  // State machine for continuous conversation
  switch (currentState) {
    case STATE_IDLE:
      if (asrChat.hasNewResult()) {
        handleASRResult();
      }
      break;

    case STATE_LISTENING:
      // Check if ASR has detected end of speech (VAD completed)
      if (asrChat.hasNewResult()) {
        handleASRResult();
      }
      break;

    case STATE_PROCESSING_LLM:
      // This state is handled in handleASRResult()
      break;

    case STATE_PLAYING_TTS:
      // This state is handled in handleASRResult()
      break;

    case STATE_WAIT_TTS_COMPLETE:
      // Check if TTS playback has completed
      // We check audio.isRunning() periodically
      if (millis() - ttsCheckTime > 100) {  // Check every 100ms
        ttsCheckTime = millis();

        if (!audio.isRunning()) {
          // TTS has completed
          Serial.println("[TTS] Playback finished");

          if (continuousMode) {
            // Restart ASR for next round
            currentState = STATE_LISTENING;

            if (asrChat.startRecording()) {
              Serial.println("\n[ASR] Listening... Speak now");
            } else {
              Serial.println("[ERROR] Failed to restart ASR");
              stopContinuousMode();
            }
          } else {
            continuousMode = false;
            currentState = STATE_IDLE;
            asrChat.startRecording();
          }
        } else {
          // Still playing, check for timeout (optional)
          if (millis() - ttsStartTime > 60000) {  // 60 second timeout
            Serial.println("[WARN] TTS timeout, forcing restart");

            if (continuousMode) {
              currentState = STATE_LISTENING;
              if (asrChat.startRecording()) {
                Serial.println("\n[ASR] Listening... Speak now");
              } else {
                stopContinuousMode();
              }
            } else {
              continuousMode = false;
              currentState = STATE_IDLE;
              asrChat.startRecording();
            }
          }
        }
      }
      break;
  }

  // Very small delay - audio capture needs high priority
  if (currentState == STATE_LISTENING) {
    // During recording, minimize delay to ensure audio data is sent fast enough
    yield();
  } else {
    // In other states, can have slightly longer delay
    yield();
  }
}
