#pragma once

// TinyML signal classifier: small int8 MLP (include/ml_model.h, trained by
// tools/ml/train.py) over spectrum + envelope features of one report.

struct MlResult {
    int cls;            // index into ml_class_name()
    float prob;         // softmax probability of cls
};

// psd: FFT_SIZE/2 averaged power bins (full scale sine = 1.0);
// env_db: 10 ms envelope in dB, env_count values.
MlResult classifier_run(const float *psd, const float *env_db, int env_count);
const char *ml_class_name(int cls);

// Last computed feature vector (for ML_LOG_FEATURES).
const float *classifier_features(int *n);
