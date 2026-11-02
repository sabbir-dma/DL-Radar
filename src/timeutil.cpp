#include "timeutil.h"
#include "storage.h"
#include "config.h"

String getTimestamp14() {
    return storage_getTimestamp14();
}

bool getAOS() {
    return deviceOnline;
}