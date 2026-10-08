// The motion sensor (QMI8658) for forge_presence's wake on pick-up, in the browser: a phone's accelerometer (devicemotion,
// read by index.html into Module.emuAccel, in g), or the page's "Pick it up" button, which tilts it for a moment.
// A computer without one lies still: 1 g straight down.
#include <emscripten.h>
#include "imu.h"

EM_JS(void, js_accel, (float *g), {
    const a = Module.emuAccel || [0, 0, 1];
    HEAPF32[(g >> 2) + 0] = a[0];
    HEAPF32[(g >> 2) + 1] = a[1];
    HEAPF32[(g >> 2) + 2] = a[2];
});

bool imu_init(i2c_master_bus_handle_t bus) { (void)bus; return true; }

bool imu_read(float g[3])
{
    js_accel(g);
    return true;
}
