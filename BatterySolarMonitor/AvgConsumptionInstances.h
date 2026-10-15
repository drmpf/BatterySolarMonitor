// AvgConsumptionInstances.h  file  =================
// The one AvgConsumptionProfile instance -- declared here, defined once
// in AvgConsumptionInstances.cpp, so both AvgConsumptionTask.h's fetch
// sequence and whatever consumes it (AvgConsumptionRefresh.cpp,
// BatteryProjection.cpp) can reach the same instance. Guarded by
// AvgConsumptionTask.h's lockAvgConsumptionProfile() -- see that file.
#ifndef AVG_CONSUMPTION_INSTANCES_H
#define AVG_CONSUMPTION_INSTANCES_H

#include "AvgConsumptionProfile.h"

extern AvgConsumptionProfile avgConsumptionProfile;

#endif
// ================= end of AvgConsumptionInstances.h  file
