#include <LiquidCrystal.h>
#include <max6675.h>
#include <math.h>

// ==========================
// LCD: RS, E, D4, D5, D6, D7
// ==========================
LiquidCrystal lcd(7, 6, 5, 4, A0, A1);

// ==========================
// MAX6675: SCK, CS, SO
// ==========================
int thermoSCK = 13;
int thermoCS  = 10;
int thermoSO  = 12;

MAX6675 thermocouple(thermoSCK, thermoCS, thermoSO);

// ==========================
// Final deployable model constants
// classes_: [0, 1, 2]
// 0 = background, 1 = water, 2 = flame
// ==========================
const int FEATURE_COUNT = 8;
const int WINDOW = 6;

const float SCALER_MEAN[8] = {
  50.0770024773f, 49.5831154234f, 0.1372832370f, 0.1415909716f,
  0.5353702175f, -0.2512386457f, 0.4700139119f, 0.2881336156f
};

const float SCALER_STD[8] = {
  19.9513292393f, 19.6420741605f, 0.5490883984f, 0.4471568452f,
  0.5261088397f, 0.4646888289f, 0.6775544422f, 0.1313555258f
};

const float LOGREG_COEF_CLASS_0[8] = {
  -1.0665727492f, -0.8158413432f, -1.2310814416f, -2.5904672804f,
  -0.4146361218f, -0.4983854282f, -1.1625120380f, 0.0800489103f
};

const float LOGREG_COEF_CLASS_1[8] = {
  0.0739980282f, 0.0898207338f, 0.1149851963f, 1.9266203110f,
  0.0574959186f, 0.4625352928f, -0.5782879439f, 0.2493090889f
};

const float LOGREG_COEF_CLASS_2[8] = {
  0.9925747210f, 0.7260206094f, 1.1160962453f, 0.6638469694f,
  0.3571402032f, 0.0358501354f, 1.7407999820f, -0.3293579992f
};

const float LOGREG_INTERCEPT[3] = {
  -0.1194657917f, 1.4108650433f, -1.2913992517f
};

// ==========================
// Sampling
// ==========================
const unsigned long SAMPLE_INTERVAL_MS = 250;

// ==========================
// Decision / display logic
// ==========================
const float START_TEMP_THRESHOLD = 35.0f;         // temperature must be above this to start analyzing
const float START_MEAN_DIFF_THRESHOLD = 0.05f;    // must be rising
const float COOLING_MEAN_DIFF_THRESHOLD = -0.03f; // falling threshold

const int VOTE_WINDOW = 5;       // keep last 5 source predictions
const int REQUIRED_VOTES = 4;    // need at least 4 same votes to lock

// smarter unlock parameters
const int REQUIRED_BKG_STREAK = 4;           // unlock if background predicted 4 times in a row
const float DROP_FROM_PEAK_THRESHOLD = 8.0f; // unlock if temp falls 8C below locked peak
const unsigned long MAX_LOCK_MS = 8000;      // force unlock logic after 8 seconds if already cooling

// ==========================
// Rolling buffer: previous 6 temps
// ==========================
float historyBuffer[WINDOW];
int validHistoryCount = 0;

// ==========================
// Decision state
// ==========================
int voteBuffer[VOTE_WINDOW];
int voteCount = 0;

bool locked = false;
int lockedClass = 0;              // 0=bkg, 1=water, 2=flame
const char* currentStatus = "MONITOR";

// new smart unlock state
float peakLockedTemp = 0.0f;      // highest temp seen after locking
int backgroundStreak = 0;         // consecutive background predictions
unsigned long lockStartMs = 0;    // when lock started

// ==========================
// LCD helpers
// ==========================
void printLine(int row, const char* text) {
  lcd.setCursor(0, row);
  lcd.print("                ");
  lcd.setCursor(0, row);
  lcd.print(text);
}

void showLCD(float tempC, const char* status) {
  lcd.setCursor(0, 0);
  lcd.print("                ");
  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print(tempC, 1);
  lcd.print((char)223);
  lcd.print("C");

  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  lcd.print(status);
}

// ==========================
// Math helpers
// ==========================
float meanArray(const float* arr, int n) {
  float s = 0.0f;
  for (int i = 0; i < n; i++) s += arr[i];
  return s / n;
}

float stdArray(const float* arr, int n, float meanVal) {
  float s = 0.0f;
  for (int i = 0; i < n; i++) {
    float d = arr[i] - meanVal;
    s += d * d;
  }
  return sqrt(s / n);
}

int argmax3(float a, float b, float c) {
  if (a >= b && a >= c) return 0;
  if (b >= a && b >= c) return 1;
  return 2;
}

// ==========================
// Vote helpers
// ==========================
void resetVotes() {
  voteCount = 0;
  for (int i = 0; i < VOTE_WINDOW; i++) {
    voteBuffer[i] = 0;
  }
}

void pushVote(int pred) {
  if (voteCount < VOTE_WINDOW) {
    voteBuffer[voteCount] = pred;
    voteCount++;
  } else {
    for (int i = 0; i < VOTE_WINDOW - 1; i++) {
      voteBuffer[i] = voteBuffer[i + 1];
    }
    voteBuffer[VOTE_WINDOW - 1] = pred;
  }
}

int majorityVote() {
  int count0 = 0, count1 = 0, count2 = 0;

  for (int i = 0; i < voteCount; i++) {
    if (voteBuffer[i] == 0) count0++;
    else if (voteBuffer[i] == 1) count1++;
    else if (voteBuffer[i] == 2) count2++;
  }

  if (count0 >= count1 && count0 >= count2) return 0;
  if (count1 >= count0 && count1 >= count2) return 1;
  return 2;
}

bool voteStrongEnough(int cls) {
  int count = 0;
  for (int i = 0; i < voteCount; i++) {
    if (voteBuffer[i] == cls) count++;
  }
  return (count >= REQUIRED_VOTES);
}

// ==========================
// Buffer helpers
// ==========================
void shiftHistoryAndAppend(float newTemp) {
  if (validHistoryCount < WINDOW) {
    historyBuffer[validHistoryCount] = newTemp;
    validHistoryCount++;
  } else {
    for (int i = 0; i < WINDOW - 1; i++) {
      historyBuffer[i] = historyBuffer[i + 1];
    }
    historyBuffer[WINDOW - 1] = newTemp;
  }
}

// ==========================
// Feature extraction
// Python matched exactly:
// window = previous 6 points
// current_temp = current point
// ==========================
void buildFeatures(float currentTemp, float features[8]) {
  float diffs[WINDOW - 1];

  for (int i = 1; i < WINDOW; i++) {
    diffs[i - 1] = historyBuffer[i] - historyBuffer[i - 1];
  }

  float ma = meanArray(historyBuffer, WINDOW);
  float currentDiff = currentTemp - historyBuffer[WINDOW - 1];
  float meanDiff = meanArray(diffs, WINDOW - 1);

  float maxDiff = diffs[0];
  float minDiff = diffs[0];
  for (int i = 1; i < WINDOW - 1; i++) {
    if (diffs[i] > maxDiff) maxDiff = diffs[i];
    if (diffs[i] < minDiff) minDiff = diffs[i];
  }

  float tempStd = stdArray(historyBuffer, WINDOW, ma);
  float diffStd = stdArray(diffs, WINDOW - 1, meanDiff);

  features[0] = currentTemp;
  features[1] = ma;
  features[2] = currentDiff;
  features[3] = meanDiff;
  features[4] = maxDiff;
  features[5] = minDiff;
  features[6] = tempStd;
  features[7] = diffStd;
}

// ==========================
// Multiclass Logistic Regression
// ==========================
int predictClass(const float features[8], float scoresOut[3]) {
  float xs[8];

  for (int i = 0; i < FEATURE_COUNT; i++) {
    xs[i] = (features[i] - SCALER_MEAN[i]) / SCALER_STD[i];
  }

  float z0 = LOGREG_INTERCEPT[0];
  float z1 = LOGREG_INTERCEPT[1];
  float z2 = LOGREG_INTERCEPT[2];

  for (int i = 0; i < FEATURE_COUNT; i++) {
    z0 += LOGREG_COEF_CLASS_0[i] * xs[i];
    z1 += LOGREG_COEF_CLASS_1[i] * xs[i];
    z2 += LOGREG_COEF_CLASS_2[i] * xs[i];
  }

  scoresOut[0] = z0;
  scoresOut[1] = z1;
  scoresOut[2] = z2;

  return argmax3(z0, z1, z2);
}

const char* className(int pred) {
  if (pred == 0) return "BKG";
  if (pred == 1) return "WATER";
  return "FLAME";
}

// ==========================
// Setup
// ==========================
void setup() {
  Serial.begin(9600);

  lcd.begin(16, 2);
  printLine(0, "Temp Detect");
  printLine(1, "Starting...");
  delay(1000);

  resetVotes();
  peakLockedTemp = 0.0f;
  backgroundStreak = 0;
  lockStartMs = 0;
}

// ==========================
// Main loop
// ==========================
void loop() {
  static unsigned long lastSample = 0;
  unsigned long nowMs = millis();

  if (nowMs - lastSample < SAMPLE_INTERVAL_MS) {
    return;
  }
  lastSample = nowMs;

  float tempC = thermocouple.readCelsius();

  // Need previous WINDOW points before first prediction
  if (validHistoryCount < WINDOW) {
    shiftHistoryAndAppend(tempC);
    currentStatus = "WARMUP";
    showLCD(tempC, currentStatus);

    Serial.print("Temp: ");
    Serial.print(tempC, 2);
    Serial.print(" | Status: WARMUP | Count=");
    Serial.println(validHistoryCount);

    return;
  }

  // Build features using previous WINDOW points + current temp
  float features[8];
  buildFeatures(tempC, features);

  float scores[3];
  int rawPred = predictClass(features, scores);

  float currentTemp = features[0];
  float meanDiff = features[3];

  bool risingCondition =
      (currentTemp > START_TEMP_THRESHOLD) &&
      (meanDiff > START_MEAN_DIFF_THRESHOLD);

  bool coolingCondition = (meanDiff < COOLING_MEAN_DIFF_THRESHOLD);

  // ==========================
  // Display / state logic
  // ==========================
  if (!locked) {
    if (risingCondition) {
      // Only vote on source classes, not background
      if (rawPred == 1 || rawPred == 2) {
        pushVote(rawPred);
      }

      currentStatus = "ANALYZE";

      if (voteCount >= VOTE_WINDOW) {
        int maj = majorityVote();
        if ((maj == 1 || maj == 2) && voteStrongEnough(maj)) {
          locked = true;
          lockedClass = maj;
          currentStatus = className(lockedClass);

          peakLockedTemp = currentTemp;
          backgroundStreak = 0;
          lockStartMs = nowMs;
        }
      }
    } else {
      resetVotes();
      currentStatus = "MONITOR";
    }
  } else {
    // already locked
    currentStatus = className(lockedClass);

    // update peak seen since lock
    if (currentTemp > peakLockedTemp) {
      peakLockedTemp = currentTemp;
    }

    // count consecutive background predictions only when not rising anymore
    if (rawPred == 0 && meanDiff <= 0.02f) {
      backgroundStreak++;
    } else {
      backgroundStreak = 0;
    }

    bool droppedEnough = (peakLockedTemp - currentTemp) >= DROP_FROM_PEAK_THRESHOLD;
    bool lockTooLong = (nowMs - lockStartMs) >= MAX_LOCK_MS;

    bool unlockCondition =
        (backgroundStreak >= REQUIRED_BKG_STREAK) ||
        (droppedEnough && meanDiff <= 0.0f) ||
        (lockTooLong && meanDiff <= 0.0f);

    if (unlockCondition) {
      locked = false;
      lockedClass = 0;
      resetVotes();
      currentStatus = "MONITOR";

      peakLockedTemp = 0.0f;
      backgroundStreak = 0;
      lockStartMs = 0;
    }
  }

  // ==========================
  // Serial debug
  // ==========================
  Serial.print("Temp: ");
  Serial.print(tempC, 2);
  Serial.print(" | RawPred: ");
  Serial.print(className(rawPred));
  Serial.print(" | LCD: ");
  Serial.print(currentStatus);
  Serial.print(" | meanDiff: ");
  Serial.print(meanDiff, 4);
  Serial.print(" | bkgStreak: ");
  Serial.print(backgroundStreak);
  Serial.print(" | peakLockedTemp: ");
  Serial.print(peakLockedTemp, 2);
  Serial.print(" | Scores: ");
  Serial.print(scores[0], 4);
  Serial.print(", ");
  Serial.print(scores[1], 4);
  Serial.print(", ");
  Serial.println(scores[2], 4);

  // LCD
  showLCD(tempC, currentStatus);

  // update history after prediction
  shiftHistoryAndAppend(tempC);
}