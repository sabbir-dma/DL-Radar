#include "timeutil.h"
#include "storage.h"
#include "config.h"

String getTimestamp14() {
    return storage_getTimestamp14();
}

bool getAOS() {
    return deviceOnline;
}

String prettyTs(const String &t) {
    if (t.length() < 12) return t;
    return t.substring(0, 2) + "-" + t.substring(2, 4) + "-" + t.substring(4, 6) + " " +
           t.substring(6, 8) + ":" + t.substring(8, 10) + ":" + t.substring(10, 12);
}

String nowPretty() {
    return prettyTs(storage_getTimestamp14());
}
