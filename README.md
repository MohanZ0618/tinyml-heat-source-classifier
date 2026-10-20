# TinyML Heat Source Classifier

An Arduino temperature-sensing project that distinguishes background, hot-water heating, and flame heating using lightweight machine learning.

The device reads a K-type thermocouple through a MAX6675 module, computes eight temporal features, and runs a three-class logistic regression model locally. Prediction voting and state locking stabilize the result shown on a 16 x 2 LCD.

## Included in this release

| File | Purpose |
| --- | --- |
| [README.md](README.md) | Design decisions, training methodology, hardware setup, and usage |
| [sketch_apr10a/sketch_apr10a.ino](sketch_apr10a/sketch_apr10a.ino) | Sensor acquisition, feature extraction, embedded inference, and LCD/serial output |

The firmware includes trained model constants and can run without a Python training environment. Training datasets and the development notebook are not included in this release, so the original training experiment cannot be reproduced from these files alone.

## Approach and technology choices

### Use temperature behavior to distinguish heating conditions

A threshold can indicate that a sensor is hot, but water and flame heating can pass through similar temperature ranges. The model therefore uses recent temperature changes and variability along with temperature level. Its scope is the three conditions represented in the development data, rather than arbitrary heat-source identification.

| Class | Development label | Serial output |
| --- | --- | --- |
| 0 | Background / no labeled heating event | `BKG` |
| 1 | Hot-water / boiling-water heating | `WATER` |
| 2 | Flame heating | `FLAME` |

### Summarize a short history with eight features

The final model uses the current temperature and six previous readings. A compact feature vector captures level, trend, and fluctuation while keeping the Arduino implementation small.

| Feature, in model order | Definition and purpose |
| --- | --- |
| `current_temp` | Current temperature; describes the present level |
| `ma` | Mean of six previous temperatures; summarizes the recent level |
| `current_diff` | Current minus previous temperature; captures the latest change |
| `mean_diff` | Mean of adjacent differences within the history; describes the recent trend |
| `max_diff` | Largest historical difference; captures a rapid increase |
| `min_diff` | Smallest historical difference; captures a decrease or slower interval |
| `temp_std` | Population standard deviation of the historical temperatures; describes variation |
| `diff_std` | Population standard deviation of the historical differences; describes variation in the rate of change |

The development experiment compared history lengths of 4, 6, 8, 10, and 12. Six gave the highest macro F1 in that recorded comparison and was retained. This choice is specific to the collected data and evaluation setup.

### Choose a small model with explicit preprocessing

Logistic regression provides an interpretable model that can be exported as a few C++ arrays. Inference requires standardization, weighted sums, and a class comparison, which makes it practical without a separate machine-learning runtime on the device.

Feature standardization matters because temperature levels and small temperature differences have different scales. The firmware uses the same learned means and standard deviations as the Python model:

```text
standardized_feature = (feature - training_mean) / training_standard_deviation
class_score = intercept + sum(coefficient * standardized_feature)
prediction = class with the largest score
```

The sketch compares linear scores directly because the display needs the winning class. The serial scores are not probabilities.

### Handle imbalance and compare more than accuracy

Background examples greatly outnumbered water and flame examples in development. Background subsampling reduced their dominance. A background-to-combined-event ratio of 2.0 gave the highest macro F1 among the ratios in the recorded comparison.

Manual inverse-frequency class weights gave more emphasis to the less frequent event classes. In the recorded comparison, weighting increased flame recall from approximately 76.6% to 85.7%, while overall accuracy changed from approximately 91.9% to 91.3%. This reflects a tradeoff between overall accuracy and detecting flame examples.

The final model uses `C = 0.1`. Smaller `C` applies stronger regularization. This is the deployed setting, rather than a claim that it maximizes every metric: the recorded regularization sweep ranked `C = 10` higher by macro F1 in that comparison.

### Stabilize the device output

Early heating and noisy readings can produce changing predictions. The firmware collects eligible source predictions and requires four matching votes in a full five-vote buffer before locking the LCD result. Cooling, background predictions, and elapsed time help release the lock for the next event.

This separates the instantaneous model prediction from the device's confirmed display state. The rules improve display stability, with a tradeoff in confirmation time.

## Hardware setup

Required components:

- An Arduino-compatible board exposing the pins below. The exact original board model is not recorded in the sketch.
- MAX6675 module and K-type thermocouple.
- Parallel-interface 16 x 2 LCD compatible with `LiquidCrystal`.
- USB cable and wiring components.

| Signal | Arduino pin |
| --- | --- |
| MAX6675 SCK | 13 |
| MAX6675 CS | 10 |
| MAX6675 SO | 12 |
| LCD RS | 7 |
| LCD E | 6 |
| LCD D4 | 5 |
| LCD D5 | 4 |
| LCD D6 | A0 |
| LCD D7 | A1 |

Complete power, ground, LCD contrast, and backlight wiring according to the documentation for the board and modules used.

## Run the classifier

1. Open `sketch_apr10a/sketch_apr10a.ino` in Arduino IDE.
2. Install `LiquidCrystal` and a MAX6675 library exposing `max6675.h` and the `MAX6675` class.
3. Select the connected board and port, verify the sketch, and upload it.
4. Open Serial Monitor at **9600 baud**.
5. Allow the device to collect six initial temperature readings.
6. Observe the thermocouple under heating conditions corresponding to the development classes. Read temperature and status on the LCD, and use serial output for diagnostics.

The nominal sampling interval is **250 ms**. It is not a measured detection latency; confirmation also depends on the temperature signal and voting history.

| LCD status | Meaning |
| --- | --- |
| `WARMUP` | Filling the initial history buffer |
| `MONITOR` | Monitoring without a confirmed heat source |
| `ANALYZE` | Heating gate is active and eligible predictions are being collected |
| `WATER` | Confirmed water classification is locked |
| `FLAME` | Confirmed flame classification is locked |

Serial output contains the raw class, display status, historical mean difference, background streak, peak temperature while locked, and three model scores.

### Confirmation and reset rules

Source voting starts when temperature is above **35 degrees C** and the historical mean difference is above **0.05 degrees C per sample**. Only water and flame predictions enter the source-vote buffer. A full buffer and at least four matching votes are required to lock a result.

The lock is released when any of these conditions is met:

- Four consecutive background predictions occur while mean difference is at most 0.02.
- Temperature falls at least 8 degrees C from the locked peak and mean difference is nonpositive.
- At least 8 seconds have elapsed since locking and mean difference is nonpositive.

Sampling, thresholds, vote settings, and unlock settings are constants near the top of the sketch. A reset of the board clears its history and decision state.

## Training methodology

The development workflow used Python, pandas, NumPy, and scikit-learn:

1. Inspect thermocouple readings and manually label background, water, and flame events.
2. Clean numeric temperature data and inspect class counts.
3. Extract the sliding-window features in the order listed above.
4. Subsample background windows and create an 80/20 stratified random evaluation split with `random_state=42`.
5. Fit a `StandardScaler` on training features and train multiclass logistic regression.
6. Compare history lengths, regularization strengths, background ratios, and class weights.
7. Evaluate the final configuration, then fit a deployment scaler and model on all selected rows.
8. Export preprocessing and model constants to Arduino C++.

For an independently trained replacement, update these definitions in the sketch:

```text
SCALER_MEAN
SCALER_STD
LOGREG_COEF_CLASS_0
LOGREG_COEF_CLASS_1
LOGREG_COEF_CLASS_2
LOGREG_INTERCEPT
```

Keep the feature order, population-standard-deviation convention, class mapping, `WINDOW`, and `FEATURE_COUNT` synchronized with the training implementation. The current firmware uses `WINDOW = 6` and `FEATURE_COUNT = 8`. Verify the updated firmware and evaluate it again on hardware after replacing the model.

## Recorded development results

The final recorded evaluation used 727 test windows:

| Metric | Result |
| --- | ---: |
| Accuracy | 91.33% |
| Macro F1 | 0.8775 |
| Background recall | 94.64% |
| Water recall | 84.24% |
| Flame recall | 85.71% |

These values were transcribed from saved development outputs. They are not a fresh rerun or an independent hardware benchmark.

The evaluation randomly split overlapping windows, which can share readings across training and test sets. Hyperparameter comparisons also reused the test split. An independent session-based evaluation is needed before claiming generalization to new collection conditions. The deployment model was refitted on all selected rows after evaluation; the table reports the evaluation model's results.

The exact board and library versions are not recorded. A fresh Arduino build and hardware validation have not been performed as part of this documentation release.

## Engineering work represented

- Sensor integration and temperature data collection.
- Event labeling and time-series feature engineering.
- Class imbalance handling and model comparisons.
- Export of preprocessing and model parameters to embedded C++.
- On-device inference, prediction voting, lock/reset logic, and LCD/serial integration.

Potential extensions include independent heating-session tests, additional training conditions, a documented hardware bill of materials, and Python-versus-Arduino feature and prediction comparisons on identical recorded inputs.
