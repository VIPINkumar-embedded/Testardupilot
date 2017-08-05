#pragma once

#include <AP_Buffer/AP_Buffer.h>
#include <AP_HAL/AP_HAL.h>
#include <AP_Math/AP_Math.h>
#include <DataFlash/DataFlash.h>
#include <GCS_MAVLink/GCS.h>

#include <bitset>
#include <vector>

#define SAFERTL_ACCURACY_DEFAULT 2.0f // how many meters to move before appending a new position to return_path
#define SAFERTL_MAX_POINTS_DEFAULT 100 // the amount of memory used by safe RTL will be slightly higher than 3*8*MAX_PATH_LEN bytes. Increasing this number will improve path pruning, but will use more memory, and running a path cleanup will take longer. No longer than 255.

#define SAFERTL_PRUNING_DELTA (SAFERTL_ACCURACY_DEFAULT * 0.99) // XXX must be smaller than position_delta! How many meters apart must two points be, such that we can assume that there is no obstacle between those points
#define SAFERTL_SIMPLIFICATION_EPSILON (SAFERTL_ACCURACY_DEFAULT * 0.5)
#define SAFERTL_MAX_DETECTABLE_LOOPS (SAFERTL_MAX_POINTS_DEFAULT / 5) // the highest amount of loops that the loop detector can detect. Should never be greater than 255
#define SAFERTL_RDP_STACK_LEN 64 // the amount of memory to be allocated for the RDP algorithm to write its to do list.
// XXX A number too small for RDP_STACK_LEN can cause a buffer overflow! The number to put here is int((s/2-1)+min(s/2, MAX_PATH_LEN-s)), where s = pow(2, floor(log(MAX_PATH_LEN)/log(2)))
// To avoid this annoying math, a good-enough overestimate is ciel(MAX_PATH_LEN*2./3.)
#define SAFERTL_SIMPLIFICATION_TIME 200
#define SAFERTL_LOOP_TIME 300
#define SAFERTL_BAD_POSITION_TIME 15000 // if the position if bad for more than 15 seconds, SafeRTL will be unavailable for the rest of the flight.

#define HYPOT(a,b) (a-b).length()

class SafeRTL_Path {

public:

    // constructor
    SafeRTL_Path(const AP_AHRS& ahrs, DataFlash_Class& dataflash, GCS& gcs, bool log);

    // turn on/off accepting new points in calls to append_if_far_enough
    void accepting_new_points(bool value) { _accepting_new_points = value; }

    bool update(bool position_ok);

    // perform thoroguh clean-up.  This should be run just before initiating the RTL. Returns a pointer to the cleaned-up path or nullptr if clean-up is not complete
    Vector3f* thorough_cleanup();

    // get a point on the path
    const Vector3f& get_point(uint32_t index) const { return path[index]; }

    // get next point on the path to home
    uint32_t pop_point(Vector3f& point);

    // clear return path and set home locatione
    void reset_path(bool position_ok, const Vector3f& start);

    bool cleanup_ready() const { return _pruning_complete && _simplification_complete; }
    bool is_active() const { return _active; }

    // the two cleanup steps. These should be run regularly, maybe even by a different thread
    void detect_simplifications();
    void detect_loops();

    // parameter var table
    static const struct AP_Param::GroupInfo var_info[];

    AP_Float safertl_accuracy;
    AP_Int32 safertl_max_points;

private:
    // add NED position (normally vehicle's current position) to the path
    void _append_if_far_enough(const Vector3f &pos);
    // perform clean-up regularly from main loop
    bool _routine_cleanup();
    // misc cleanup helper methods:
    void _reset_simplification();
    void _reset_pruning();
    void _zero_points_by_simplification_bitmask();
    void _remove_unacceptable_overlapping_loops();
    void _zero_points_by_loops(uint32_t points_to_delete);
    void _remove_empty_points();
    void _remove_empty_loops();
    // _segment_segment_dist returns two things, the closest distance reached between 2 line segments, and the point exactly between them.
    typedef struct {
        float distance;
        Vector3f point;
    } dist_point;
    // typedef struct dist_point dist_point;
    static dist_point _segment_segment_dist(const Vector3f& p1, const Vector3f& p2, const Vector3f& p3, const Vector3f& p4);
    static float _point_line_dist(const Vector3f& point, const Vector3f& line1, const Vector3f& line2);

    const AP_AHRS& _ahrs;
    DataFlash_Class& _dataflash;
    GCS& _gcs;

    bool _logging_enabled;
    bool _active; // if the path becomes too long to keep in memory, and too convoluted to be cleaned up, SafeRTL will be permanently deactivated (for the remainder of the flight)
    uint32_t _time_of_last_good_position;

    // Simplification state
    bool _simplification_complete;
    // structure and buffer to hold the "to-do list" for the RDP algorithm.
    typedef struct {
        uint32_t start;
        uint32_t finish;
    } start_finish;
    AP_Buffer<start_finish,SAFERTL_RDP_STACK_LEN> _simplification_stack;
    // the result of the simplification algorithm
    std::bitset<SAFERTL_MAX_POINTS_DEFAULT> _simplification_bitmask;
    // everything before _simplification_clean_until has been calculated already to be un-simplify-able. This avoids recalculating a known result.
    uint32_t _simplification_clean_until;

    // points are stored in meters from EKF origin in NED
    Vector3f path[SAFERTL_MAX_POINTS_DEFAULT];
    uint32_t _last_index;
    bool _accepting_new_points; // false means that any call to append_if_far_enough() will fail. This should be unset when entering SafeRTL mode, and set when exiting.

    // Pruning state
    bool _pruning_complete;
    uint32_t _pruning_current_i;
    uint32_t _pruning_min_j;
    typedef struct {
        uint32_t start_index;
        uint32_t end_index;
        Vector3f halfway_point;
    } loop;
    // the result of the pruning algorithm
    loop _prunable_loops[SAFERTL_MAX_DETECTABLE_LOOPS]; // This is where info about detected loops is stored.
    int32_t _prunable_loops_last_index;
    // everything before _pruning_clean_until has been calculated already to be un-simplify-able. This avoids recalculating a known result.
    uint32_t _pruning_clean_until;
};
