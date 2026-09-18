//Assign Teensy's serial number here
constexpr int cameraID = 1;

enum DebugLevel {
    DBG_NONE = 0,
    DBG_ERROR = 1,
    DBG_INFO = 2,
    DBG_VERBOSE = 3
};

extern DebugLevel debugLevel;