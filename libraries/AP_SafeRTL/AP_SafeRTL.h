#pragma once

#include <AP_Buffer/AP_Buffer.h>
#include <AP_HAL/AP_HAL.h>
#include <AP_Common/Bitmask.h>
#include <AP_Math/AP_Math.h>
#include <AP_AHRS/AP_AHRS.h>
#include <DataFlash/DataFlash.h>
#include <GCS_MAVLink/GCS.h>

// definitions and macros
#define SAFERTL_ACCURACY_DEFAULT        2.0f    // default _ACCURACY parameter value.  Points will be no closer than this distance (in meters) together.
#define SAFERTL_POINTS_DEFAULT          150     // default _POINTS parameter value.  High numbers improve path pruning but use more memory and CPU for cleanup. Memory used will be 20bytes * this number.
#define SAFERTL_POINTS_MAX              500     // the absolute maximum number of points this library can support.
#define SAFERTL_BAD_POSITION_TIMEOUT    15000   // the time in milliseconds with no valid position, before SafeRTL is disabled for the flight
#define SAFERTL_CLEANUP_START_MARGIN    10      // routine cleanup algorithms begin when the path array has only this many empty slots remaining
#define SAFERTL_CLEANUP_POINT_MIN       10      // cleanup algorithms will remove points if they remove at least this many points
#define SAFERTL_SIMPLIFY_EPSILON (_accuracy * 0.5f)
#define SAFERTL_SIMPLIFY_STACK_LEN_MULT (2.0f/3.0f)+1   // simplify buffer size as compared to maximum number of points.
                                                // The minimum is int((s/2-1)+min(s/2, SAFERTL_POINTS_MAX-s)), where s = pow(2, floor(log(SAFERTL_POINTS_MAX)/log(2)))
                                                // To avoid this annoying math, a good-enough overestimate is ceil(SAFERTL_POINTS_MAX*2.0f/3.0f)
#define SAFERTL_SIMPLIFY_TIME_US        200 // maximum time (in microseconds) the simplification algorithm will run before returning
#define SAFERTL_PRUNING_DELTA (_accuracy * 0.99) // How many meters apart must two points be, such that we can assume that there is no obstacle between them.  must be smaller than _ACCURACY parameter
#define SAFERTL_PRUNING_LOOP_BUFFER_LEN_MULT 0.25f // pruning loop buffer size as compared to maximum number of points
#define SAFERTL_LOOP_TIME_US            300 // maximum time (in microseconds) that the loop finding algorithm will run before returning
#define HYPOT(a,b)                      (a-b).length()  // macro to calculate length between two points

class AP_SafeRTL {

public:

    // constructor
    AP_SafeRTL(const AP_AHRS& ahrs);

    // initialise safe rtl including setting up background processes
    void init();

    // clear return path and set return location is position_ok is true.  This should be called as part of the arming procedure
    // if position_ok is false, SafeRTL will not be available.
    // example sketches use method that allows providing vehicle position directly
    void reset_path(bool position_ok);
    void reset_path(bool position_ok, const Vector3f& current_pos);

    // call this a couple of times per second regardless of what mode the vehicle is in
    // example sketches use method that allows providing vehicle position directly
    void update(bool position_ok, bool save_position);
    void update(bool position_ok, const Vector3f& current_pos);

    // return true if safe_rtl is usable (it may become unusable if the user took off without GPS lock or the path became too long)
    bool is_active() const { return _active; }

    // perform thorough cleanup including simplification, pruning and removal of all unnecessary points
    // returns true if the thorough cleanup was completed, false if it has not yet completed
    // this method should be called repeatedly until it returns true before initiating the return journey
    bool thorough_cleanup();

    // get a point on the path
    const Vector3f& get_point(uint16_t index) const { return _path[index]; }

    // get next point on the path to home, returns true on success
    bool pop_point(Vector3f& point);

    // returns number of points on the path
    uint16_t get_num_points();

    // the two cleanup steps. These are run regularly from the IO thread
    // these are public so that they can be tested by the example sketch
    void detect_simplifications();
    void detect_loops();

    // parameter var table
    static const struct AP_Param::GroupInfo var_info[];

private:

    // enums for logging latest actions
    enum SRTL_Actions {
        SRTL_POINT_ADD,
        SRTL_POINT_PRUNE,
        SRTL_POINT_SIMPLIFY,
        SRTL_DEACTIVATED_INIT_FAILED,
        SRTL_DEACTIVATED_BAD_POSITION,
        SRTL_DEACTIVATED_CLEANUP_FAILED
    };

    // perform clean-up regularly from main loop
    bool routine_cleanup();

    // restart simplification algorithm, should be called whenever a new point is added
    void restart_simplification();

    // reset simplification algorithm so that it will re-check all points in the path
    // should be called if the existing path is altered for example when a loop as been removed
    void reset_simplification();

    // restart pruning algorithm, should be called whenever a new point is added
    void restart_pruning();

    // reset pruning algorithm so that it will re-check all points in the path
    // should be called if the existing path is altered for example when a loop as been removed
    void reset_pruning();

    void zero_points_by_simplify_bitmask();
    void zero_points_by_loops(uint16_t points_to_delete);
    void remove_empty_points();

    // dist_point holds the closest distance reached between 2 line segments, and the point exactly between them
    typedef struct {
        float distance;
        Vector3f midpoint;
    } dist_point;

    // get the closest distance between 2 line segments and the point midway between the closest points
    static dist_point segment_segment_dist(const Vector3f& p1, const Vector3f& p2, const Vector3f& p3, const Vector3f& p4);
    static float point_line_dist(const Vector3f& point, const Vector3f& line1, const Vector3f& line2);

    // logging
    void log_action(SRTL_Actions action, const Vector3f point = Vector3f());

    // external references
    const AP_AHRS& _ahrs;

    // parameters
    AP_Float _accuracy;
    AP_Int16 _points_max;

    // SafeRTL State Variables
    bool _active;       // true if safeRTL is usable.  may become unusable if the path becomes too long to keep in memory, and too convoluted to be cleaned up, SafeRTL will be permanently deactivated (for the remainder of the flight)
    uint32_t _last_good_position_ms;    // the last system time a last good position was reported. If no position is available for a while, SafeRTL will be disabled.

    // path variables
    Vector3f* _path;    // points are stored in meters from EKF origin in NED
    uint16_t _path_points_max;  // after the array has been allocated, we will need to know how big it is. We can't use the parameter, because a user could change the parameter in-flight
    uint16_t _path_points_count;// number of points in the path array

    // Simplify state
    bool _simplify_complete;
    // structure and buffer to hold the "to-do list" for the SIMPLIFICATION algorithm.
    typedef struct {
        uint16_t start;
        uint16_t finish;
    } simplify_start_finish_t;
    simplify_start_finish_t* _simplify_stack;
    uint16_t _simplify_stack_max;   // maximum number of elements in the _simplify_stack array
    uint16_t _simplify_stack_count; // number of elements in _simplify_stack array
    Bitmask _simplify_bitmask = Bitmask(SAFERTL_POINTS_MAX);  // simplify algorithm clears bits for each point that can be removed
    uint16_t _simplify_clean_until; // all elements in _path before this index have been checked for simplification. This avoids recalculating a known result.

    // Pruning state
    bool _prune_complete;
    uint16_t _prune_current_i;
    uint16_t _prune_min_j;
    typedef struct {
        uint16_t start_index;
        uint16_t end_index;
        Vector3f midpoint;
    } prune_loop_t;
    prune_loop_t* _prunable_loops;  // the result of the pruning algorithm
    uint16_t _prunable_loops_max;   // maximum number of elements in the _prunable_loops array
    uint16_t _prunable_loops_count; // number of elements in the _prunable_loops array
    uint16_t _prune_clean_until;    // all elements in _path before this index have been checked for loops. This avoids recalculating a known result.
};
